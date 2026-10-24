#include "spotify.h"

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"

#include "AccessKeyFetcher.h"
#include "BellLogger.h"
#include "CSpotContext.h"
#include "ConstantParameters.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"

extern "C" {
#include "settings.h"
}

#define DEVICE_NAME    "Новогодняя звезда"
#define FRAME_BYTES    4                /* 16-bit stereo */
#define RING_SIZE      (384 * 1024)     /* ~2.2 s at 44.1 kHz */
#define PAUSE_HOLD_MS  (2 * 60 * 1000)  /* then the star goes back to its own playlist / radio */
#define MAX_BODY       4096             /* addUser request */
#define IDLE_POLL_MS   500
#define CONNECT_TRIES  3

static const char *TAG = "spotify";

/* ---------- cspot log -> ESP log ---------- */

class EspLogger : public bell::AbstractLogger {
    static void out(esp_log_level_t level, const std::string &file, int line, const char *format,
                    va_list args)
    {
        if (level > esp_log_level_get("cspot")) {
            return; /* cspot logs a lot at debug level */
        }
        char msg[192];
        vsnprintf(msg, sizeof(msg), format, args);
        size_t slash = file.rfind('/');
        const char *base = file.c_str() + (slash == std::string::npos ? 0 : slash + 1);
        ESP_LOG_LEVEL(level, "cspot", "%s:%d %s", base, line, msg);
    }

public:
    void debug(std::string filename, int line, std::string submodule, const char *format, ...) override
    {
        va_list args;
        va_start(args, format);
        out(ESP_LOG_DEBUG, filename, line, format, args);
        va_end(args);
    }
    void error(std::string filename, int line, std::string submodule, const char *format, ...) override
    {
        va_list args;
        va_start(args, format);
        out(ESP_LOG_ERROR, filename, line, format, args);
        va_end(args);
    }
    void info(std::string filename, int line, std::string submodule, const char *format, ...) override
    {
        va_list args;
        va_start(args, format);
        out(ESP_LOG_INFO, filename, line, format, args);
        va_end(args);
    }
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ---------- decoded audio: PSRAM ring buffer between the cspot decoder and the player ---------- */

/* Track switches are noticed by their audio reaching the speaker: the decoder runs up to
 * RING_SIZE ahead, and Spotify must show the new track only when it is actually heard. */
static struct {
    std::mutex lock;
    uint8_t *buf;
    uint64_t written, read; /* byte counters; the ring holds written - read */
    std::string stream_track; /* being decoded */
    std::string play_track;   /* being heard */
    bool marker_pending;      /* stream_track starts at marker but is not heard yet */
    uint64_t marker;
} ring;

/* Drop what is buffered (seek, skip, new playlist). A pending track start stays pending:
 * the next audio heard belongs to it or to a later one. */
static void ring_flush(void)
{
    std::lock_guard<std::mutex> g(ring.lock);
    ring.read = ring.written;
    if (ring.marker_pending && ring.marker > ring.read) {
        ring.marker = ring.read;
    }
}

/* TrackPlayer's data callback: takes what fits, 0 makes the decoder retry in 50 ms */
static size_t ring_write(uint8_t *data, size_t len, std::string_view track)
{
    std::lock_guard<std::mutex> g(ring.lock);
    if (ring.stream_track != track) {
        /* One track change at a time: the previous one must be heard first */
        if (ring.play_track != track && ring.play_track != ring.stream_track) {
            return 0;
        }
        ESP_LOGI(TAG, "Decoding next track");
        ring.stream_track = track;
        ring.marker = ring.written;
        ring.marker_pending = true;
    }
    size_t space = RING_SIZE - (size_t)(ring.written - ring.read);
    size_t n = len < space ? len : space;
    size_t pos = ring.written % RING_SIZE;
    size_t first = n < RING_SIZE - pos ? n : RING_SIZE - pos;
    memcpy(ring.buf + pos, data, first);
    memcpy(ring.buf, data + first, n - first);
    ring.written += n;
    return n;
}

static size_t ring_buffered(void)
{
    std::lock_guard<std::mutex> g(ring.lock);
    return ring.written - ring.read;
}

/* ---------- session state ---------- */

static httpd_handle_t server;
static TaskHandle_t task;

/* Zeroconf: the blob whose public key the app got from getInfo decodes its addUser */
static std::mutex zc_lock;
static std::shared_ptr<cspot::LoginBlob> zc_blob;
static std::map<std::string, std::string> zc_query; /* addUser fields waiting for the task */
static bool zc_pending;

static std::shared_ptr<cspot::SpircHandler> spirc;
static std::atomic<spotify_state_t> state{ SPOTIFY_OFF };
static std::atomic<bool> linked, playing, paused;
static std::atomic<int64_t> paused_at;
static std::atomic<bool> abort_session, stop_requested, push_volume, track_end;
static std::atomic<bool> login_failed, key_failed;
static std::atomic<int> volume{ MAX_VOLUME * 7 / 10 }; /* Spotify scale 0..65535 */
static std::atomic<int> start_offset_ms; /* position the loaded track starts at */
static std::atomic<bool> flushed; /* the queue was replaced: its first track needs no notify */
static std::atomic<bool> notify_start{ true };

static std::mutex info_lock;
static std::string user, title, artist;

static void session_event(std::unique_ptr<cspot::SpircHandler::Event> event)
{
    using Type = cspot::SpircHandler::EventType;
    switch (event->eventType) {
    case Type::PLAYBACK_START: {
        {
            std::lock_guard<std::mutex> g(ring.lock);
            ring.read = ring.written;
            ring.play_track.clear();
            ring.stream_track.clear();
            ring.marker_pending = false;
        }
        start_offset_ms = std::get<int>(event->data);
        notify_start = !flushed.exchange(false);
        track_end = false;
        playing = true;
        push_volume = true; /* Spotify does not send it at connection */
        ESP_LOGI(TAG, "Playback starts at %d ms", start_offset_ms.load());
        break;
    }
    case Type::PLAY_PAUSE: {
        bool p = std::get<bool>(event->data);
        if (p && !paused) {
            paused_at = now_ms(); /* not when confirming spotify_stop(), which set it already */
        }
        paused = p;
        ESP_LOGI(TAG, "%s", p ? "Paused" : "Playing");
        break;
    }
    case Type::TRACK_INFO: {
        auto info = std::get<cspot::TrackInfo>(event->data);
        std::lock_guard<std::mutex> g(info_lock);
        title = info.name;
        artist = info.artist;
        ESP_LOGI(TAG, "Track: %s - %s", artist.c_str(), title.c_str());
        break;
    }
    case Type::FLUSH:
        flushed = true;
        ring_flush();
        break;
    case Type::NEXT:
    case Type::PREV:
    case Type::SEEK:
        ring_flush();
        break;
    case Type::DEPLETED:
        ESP_LOGI(TAG, "Playlist ended");
        track_end = true;
        break;
    case Type::VOLUME:
        volume = std::get<int>(event->data);
        break;
    case Type::DISC:
        ESP_LOGI(TAG, "Another device took over");
        abort_session = true;
        break;
    default:
        break;
    }
}

/* Called from the session loop: tell Spotify when a new track's audio is heard and when the
 * last one has played out */
static void session_poll(void)
{
    std::string reached;
    {
        std::lock_guard<std::mutex> g(ring.lock);
        if (ring.marker_pending && ring.read > ring.marker) {
            ring.marker_pending = false;
            ring.play_track = ring.stream_track;
            reached = ring.play_track;
        }
    }
    if (!reached.empty()) {
        if (notify_start) {
            spirc->notifyAudioReachedPlayback(reached);
        } else {
            notify_start = true;
        }
        spirc->updatePositionMs(start_offset_ms);
        start_offset_ms = 0;
    }
    if (track_end && ring_buffered() < FRAME_BYTES) {
        track_end = false;
        playing = false;
        spirc->notifyAudioEnded();
    }
    if (push_volume.exchange(false)) {
        spirc->setRemoteVolume(volume);
    }
    if (stop_requested.exchange(false)) {
        spirc->setPause(true);
    }
}

/* ---------- mDNS ---------- */

static bool keys_set(const cast_settings_t &c)
{
    return c.spotify_client_id[0] && c.spotify_client_secret[0];
}

static void advertise(bool on)
{
    static bool advertised;
    if (on == advertised) {
        return;
    }
    if (on) {
        mdns_txt_item_t txt[] = { { "VERSION", "1.0" }, { "CPath", "/spotify_info" }, { "Stack", "SP" } };
        if (mdns_service_add(DEVICE_NAME, "_spotify-connect", "_tcp", 80, txt, 3) == ESP_OK) {
            ESP_LOGI(TAG, "Announced as \"%s\"", DEVICE_NAME);
        }
    } else {
        mdns_service_remove("_spotify-connect", "_tcp");
    }
    advertised = on;
}

/* ---------- session task ---------- */

/* An addUser request decoded into its own blob; null when there is none or it is broken */
static std::shared_ptr<cspot::LoginBlob> take_login(void)
{
    std::lock_guard<std::mutex> g(zc_lock);
    if (!zc_pending) {
        return nullptr;
    }
    zc_pending = false;
    try {
        zc_blob->loadZeroconfQuery(zc_query);
        /* A copy for the session: the zeroconf blob keeps answering the next requests */
        auto blob = std::make_shared<cspot::LoginBlob>(DEVICE_NAME);
        blob->loadJson(zc_blob->toJson());
        return blob;
    } catch (const std::exception &e) {
        ESP_LOGW(TAG, "Bad addUser request: %s", e.what());
        return nullptr;
    }
}

static void run_session(std::shared_ptr<cspot::LoginBlob> blob, const cast_settings_t &cast)
{
    ESP_LOGI(TAG, "Logging in as %s", blob->getUserName().c_str());
    state = SPOTIFY_CONNECTING;
    {
        std::lock_guard<std::mutex> g(info_lock);
        user = blob->getUserName();
        title.clear();
        artist.clear();
    }

    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;
    ctx->config.clientId = cast.spotify_client_id;
    ctx->config.clientSecret = cast.spotify_client_secret;
    ctx->config.volume = volume;

    /* Network errors are retried, a declined login is final */
    bool connected = false, declined = false;
    for (int i = 0; i < CONNECT_TRIES && !connected && !declined; i++) {
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        try {
            ctx->session->connectWithRandomAp();
            ctx->config.authData = ctx->session->authenticate(blob);
            connected = !ctx->config.authData.empty();
            declined = !connected;
        } catch (const std::exception &e) {
            ESP_LOGW(TAG, "Connection failed: %s", e.what());
        }
    }
    login_failed = !connected;
    if (!connected) {
        ESP_LOGE(TAG, "Login failed");
        return;
    }

    {
        std::lock_guard<std::mutex> g(ring.lock);
        ring.read = ring.written;
        ring.play_track.clear();
        ring.stream_track.clear();
        ring.marker_pending = false;
    }
    playing = paused = track_end = abort_session = stop_requested = flushed = false;
    notify_start = true;

    spirc = std::make_shared<cspot::SpircHandler>(ctx);
    spirc->getTrackPlayer()->setDataCallback(ring_write);
    spirc->setEventHandler(session_event);
    ctx->session->startTask();
    linked = true;
    ESP_LOGI(TAG, "Logged in");

    while (!abort_session) {
        ctx->session->handlePacket(); /* waits up to 200 ms */
        session_poll();
        state = !playing ? SPOTIFY_CONNECTED : paused ? SPOTIFY_PAUSED : SPOTIFY_PLAYING;

        cast_settings_t now;
        settings_get_cast(&now);
        if (!now.spotify || strcmp(now.spotify_client_id, cast.spotify_client_id) != 0 ||
            strcmp(now.spotify_client_secret, cast.spotify_client_secret) != 0) {
            ESP_LOGI(TAG, "Settings changed, logging out");
            break;
        }
        /* Another account picked the star in its app (the same one only re-sends its login) */
        std::lock_guard<std::mutex> g(zc_lock);
        if (zc_pending && zc_query["userName"] != blob->getUserName()) {
            ESP_LOGI(TAG, "Another account logs in");
            break;
        }
        zc_pending = false;
    }

    linked = false;
    playing = false;
    spirc->disconnect();
    spirc.reset();
    ring_flush();
    ESP_LOGI(TAG, "Logged out");
}

static void session_task(void *arg)
{
    for (;;) {
        cast_settings_t cast;
        settings_get_cast(&cast);
        advertise(cast.spotify && keys_set(cast));
        if (!cast.spotify || !keys_set(cast)) {
            state = cast.spotify ? SPOTIFY_NO_KEYS : SPOTIFY_OFF;
            std::lock_guard<std::mutex> g(zc_lock);
            zc_pending = false;
        } else {
            auto blob = take_login();
            if (blob) {
                run_session(blob, cast);
                continue; /* a login that ended the session may be waiting */
            }
            state = SPOTIFY_WAITING;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_POLL_MS));
    }
}

/* ---------- zeroconf HTTP ---------- */

static esp_err_t send_json(httpd_req_t *req, const std::string &body)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

static esp_err_t info_get(httpd_req_t *req)
{
    std::string body;
    {
        std::lock_guard<std::mutex> g(zc_lock);
        body = zc_blob->buildZeroconfInfo();
    }
    return send_json(req, body);
}

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out += (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

/* addUser: the app hands over the account. Decoding needs the DH math and a big stack, so it
 * happens in the session task; the app only needs to hear "OK". */
static esp_err_t add_user_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int n = httpd_req_recv(req, &body[got], body.size() - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }

    std::map<std::string, std::string> query;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t end = body.find('&', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string pair = body.substr(pos, end - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            query[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = end + 1;
    }

    cast_settings_t cast;
    settings_get_cast(&cast);
    bool accepted = cast.spotify && keys_set(cast) && query["action"] == "addUser" &&
                    !query["userName"].empty() && !query["blob"].empty() && !query["clientKey"].empty();
    if (accepted) {
        ESP_LOGI(TAG, "Login from the app: %s", query["userName"].c_str());
        std::lock_guard<std::mutex> g(zc_lock);
        zc_query = std::move(query);
        zc_pending = true;
        xTaskNotifyGive(task);
    }
    return send_json(req, accepted ? "{\"status\":101,\"spotifyError\":0,\"statusString\":\"OK\"}"
                                   : "{\"status\":202,\"spotifyError\":0,\"statusString\":\"ERROR-LOGIN-FAILED\"}");
}

/* ---------- public ---------- */

void spotify_start(httpd_handle_t httpd)
{
    server = httpd;
    static EspLogger logger;
    bell::bellGlobalLogger = &logger;
    cspot::AccessKeyFetcher::resultHandler = [](bool ok) { key_failed = !ok; };

    ring.buf = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
    zc_blob = std::make_shared<cspot::LoginBlob>(DEVICE_NAME);
    if (ring.buf == nullptr) {
        ESP_LOGE(TAG, "No memory for the audio buffer, Spotify is off");
        return;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/spotify_info", .method = HTTP_GET, .handler = info_get, .user_ctx = nullptr },
        { .uri = "/spotify_info", .method = HTTP_POST, .handler = add_user_post, .user_ctx = nullptr },
    };
    for (const auto &u : uris) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
    }
    /* Stack in PSRAM: TLS handshakes and protobuf need a lot, and it never touches the flash */
    xTaskCreateWithCaps(session_task, "spotify", 16384, nullptr, 3, &task, MALLOC_CAP_SPIRAM);
}

bool spotify_active(void)
{
    return linked && playing && (!paused || now_ms() - paused_at < PAUSE_HOLD_MS);
}

size_t spotify_read(int16_t *pcm, size_t max_frames, int timeout_ms)
{
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        if (!paused) {
            std::lock_guard<std::mutex> g(ring.lock);
            size_t n = (ring.written - ring.read) / FRAME_BYTES;
            if (n) {
                n = n < max_frames ? n : max_frames;
                size_t bytes = n * FRAME_BYTES;
                size_t pos = ring.read % RING_SIZE;
                size_t first = bytes < RING_SIZE - pos ? bytes : RING_SIZE - pos;
                memcpy(pcm, ring.buf + pos, first);
                memcpy((uint8_t *)pcm + first, ring.buf, bytes - first);
                ring.read += bytes;
                return n;
            }
        }
        if (now_ms() >= deadline) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

uint8_t spotify_volume(void)
{
    return (uint8_t)((int64_t)volume * 100 / (MAX_VOLUME - 1));
}

/* Valid UTF-8 cut to fit size bytes */
static void copy_utf8(char *out, size_t size, const std::string &s)
{
    size_t n = s.size() < size - 1 ? s.size() : size - 1;
    while (n > 0 && n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--; /* do not split a character */
    }
    memcpy(out, s.data(), n);
    out[n] = '\0';
}

void spotify_get_status(spotify_status_t *out)
{
    out->state = state;
    out->login_failed = login_failed;
    out->key_failed = key_failed;
    std::lock_guard<std::mutex> g(info_lock);
    copy_utf8(out->user, sizeof(out->user), user);
    copy_utf8(out->title, sizeof(out->title), title);
    copy_utf8(out->artist, sizeof(out->artist), artist);
}

void spotify_stop(void)
{
    if (linked && playing) {
        paused = true;
        paused_at = now_ms() - PAUSE_HOLD_MS; /* let the star go silent at once */
        stop_requested = true;
    }
}

static const char *state_name(spotify_state_t s)
{
    switch (s) {
    case SPOTIFY_OFF: return "off";
    case SPOTIFY_NO_KEYS: return "no_keys";
    case SPOTIFY_WAITING: return "waiting";
    case SPOTIFY_CONNECTING: return "connecting";
    case SPOTIFY_CONNECTED: return "connected";
    case SPOTIFY_PLAYING: return "playing";
    case SPOTIFY_PAUSED: return "paused";
    }
    return "unknown";
}

void spotify_status_json(cJSON *out)
{
    static spotify_status_t st; /* only the httpd task gets here */
    spotify_get_status(&st);
    cJSON_AddStringToObject(out, "state", state_name(st.state));
    cJSON_AddBoolToObject(out, "login_failed", st.login_failed);
    cJSON_AddBoolToObject(out, "key_failed", st.key_failed);
    cJSON_AddStringToObject(out, "user", st.user);
    cJSON_AddStringToObject(out, "title", st.title);
    cJSON_AddStringToObject(out, "artist", st.artist);
    cJSON_AddNumberToObject(out, "volume", spotify_volume());
}

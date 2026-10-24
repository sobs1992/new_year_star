/*
 * AirPlay 1 (RAOP) receiver.
 *
 * The sender finds "_raop._tcp" over mDNS and talks RTSP on TCP: ANNOUNCE carries the
 * stream format and the AES key (RSA-encrypted with the well-known AirPort Express key, as in
 * every open-source receiver), SETUP agrees on three UDP ports, RECORD starts the stream.
 * Audio comes as RTP packets of 352 stereo frames, AES-128-CBC encrypted ALAC or plain PCM.
 * Packets go into a jitter buffer by sequence number; the player takes them out in order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_audio_dec.h"
#include "esp_alac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mdns.h"
#include "psa/crypto.h"
#include "airplay.h"
#include "settings.h"

#define SERVICE_NAME    "Новогодняя звезда"
#define RTSP_PORT       5000
#define FRAMES          352            /* per packet */
#define PACKET_BYTES    (FRAMES * 4)   /* stereo 16 bit */
#define SLOTS           512            /* jitter buffer, ~4 s; power of two */
#define START_PACKETS   88             /* ~0.7 s buffered before playing */
#define MAX_UDP         2048
#define RTSP_BUF        4096
#define RTSP_BODY_MAX   4096           /* bigger bodies (cover art) are skipped */
#define TIMING_MS       3000

static const char *TAG = "airplay";

/* AirPort Express private key, public knowledge since 2011 (shairport, shairport-sync, ...) */
static const char AIRPORT_KEY[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----\n";

typedef struct {
    uint16_t seq;
    bool ready;
    uint16_t frames;
    int16_t pcm[FRAMES * 2];
} slot_t;

typedef enum {
    CODEC_ALAC,
    CODEC_PCM, /* L16, big-endian */
} stream_codec_t;

static uint8_t mac[6];
static SemaphoreHandle_t lock;          /* session + jitter buffer */
static volatile bool active;           /* RECORD received, not torn down */
static volatile bool stop_requested;
static uint8_t volume = 60;

/* Session, set up by ANNOUNCE / SETUP */
static stream_codec_t codec;
static uint8_t alac_cookie[24];
static bool encrypted;
static psa_key_id_t aes_key;
static uint8_t aes_iv[16];
static esp_audio_dec_handle_t alac_dec;
static struct sockaddr_in client;       /* sender address, ports below */
static uint16_t client_control_port, client_timing_port;
/* Opened once and never closed: closing a socket while another task waits on it in select()
 * hangs the VFS select. Packets outside a session are just dropped. */
static int audio_sock = -1, control_sock = -1, timing_sock = -1;
static uint16_t audio_port, control_port, timing_port;

/* Jitter buffer */
static slot_t *slots;
static bool have_seq;     /* read_seq / next_seq are valid */
static bool started;      /* playout running */
static uint16_t read_seq; /* next packet the player takes */
static uint16_t next_seq; /* next packet expected from the network */
static uint16_t resend_counter;
static airplay_stats_t stats;

/* ---------- crypto ---------- */

/* RSA with the AirPort key: PKCS#1 v1.5 raw signature for the challenge, OAEP-SHA1 for the
 * AES key. Mbed TLS 4 only does RSA through PSA. */
static size_t rsa_apply(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, bool sign)
{
    psa_algorithm_t alg = sign ? PSA_ALG_RSA_PKCS1V15_SIGN_RAW : PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1);
    psa_key_usage_t usage = sign ? PSA_KEY_USAGE_SIGN_HASH : PSA_KEY_USAGE_DECRYPT;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    mbedtls_pk_context pk;
    size_t out_len = 0;

    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_key(&pk, (const unsigned char *)AIRPORT_KEY, sizeof(AIRPORT_KEY), NULL, 0);
    if (rc == 0) {
        rc = mbedtls_pk_get_psa_attributes(&pk, usage, &attr);
    }
    if (rc == 0) {
        psa_set_key_algorithm(&attr, alg);
        rc = mbedtls_pk_import_into_psa(&pk, &attr, &key);
    }
    psa_status_t st = PSA_ERROR_GENERIC_ERROR;
    if (rc == 0) {
        st = sign ? psa_sign_hash(key, alg, in, in_len, out, out_size, &out_len)
                  : psa_asymmetric_decrypt(key, alg, in, in_len, NULL, 0, out, out_size, &out_len);
    }
    if (rc != 0 || st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "RSA %s failed (%d / %d)", sign ? "sign" : "decrypt", rc, (int)st);
        out_len = 0;
    }
    psa_destroy_key(key);
    psa_reset_key_attributes(&attr);
    mbedtls_pk_free(&pk);
    return out_len;
}

/* Base64 as AirPlay sends it: often without '=' padding */
static size_t b64_decode(const char *in, uint8_t *out, size_t out_size)
{
    char padded[512];
    size_t n = strcspn(in, "\r\n ");
    if (n + 4 > sizeof(padded)) {
        return 0;
    }
    memcpy(padded, in, n);
    while (n % 4) {
        padded[n++] = '=';
    }
    size_t len = 0;
    return mbedtls_base64_decode(out, out_size, &len, (const unsigned char *)padded, n) == 0 ? len : 0;
}

static void set_aes_key(const uint8_t key[16])
{
    if (aes_key) {
        psa_destroy_key(aes_key);
        aes_key = 0;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CBC_NO_PADDING);
    if (psa_import_key(&attr, key, 16, &aes_key) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "AES key import failed");
        aes_key = 0;
    }
}

/* Every packet is encrypted from the session IV; a tail shorter than a block stays plain */
static bool decrypt_payload(uint8_t *data, size_t len)
{
    size_t blocks = len & ~(size_t)15;
    if (!encrypted || blocks == 0) {
        return true;
    }
    static uint8_t in[16 + MAX_UDP];
    static uint8_t out[MAX_UDP];
    memcpy(in, aes_iv, 16);
    memcpy(in + 16, data, blocks);
    size_t out_len = 0;
    if (psa_cipher_decrypt(aes_key, PSA_ALG_CBC_NO_PADDING, in, 16 + blocks, out, sizeof(out), &out_len) != PSA_SUCCESS) {
        return false;
    }
    memcpy(data, out, out_len);
    return true;
}

/* ---------- jitter buffer ---------- */

static void buffer_reset(void)
{
    for (int i = 0; i < SLOTS; i++) {
        slots[i].ready = false;
    }
    have_seq = false;
    started = false;
}

/* How many packets the network is ahead of the player */
static int16_t ahead(void)
{
    return have_seq ? (int16_t)(next_seq - read_seq) : 0;
}

static void request_resend(uint16_t first, uint16_t count)
{
    if (control_sock < 0 || client_control_port == 0 || count == 0) {
        return;
    }
    uint8_t pkt[8] = { 0x80, 0xD5, resend_counter >> 8, resend_counter & 0xFF,
                       first >> 8, first & 0xFF, count >> 8, count & 0xFF };
    resend_counter++;
    struct sockaddr_in to = client;
    to.sin_port = htons(client_control_port);
    sendto(control_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
}

/* Decode one RTP payload into the slot of its sequence number */
static void store_packet(uint16_t seq, uint8_t *payload, size_t len, bool resent)
{
    if (!decrypt_payload(payload, len)) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!have_seq) {
        read_seq = next_seq = seq;
        have_seq = true;
    }
    int16_t from_reader = (int16_t)(seq - read_seq);
    if (from_reader < 0 || from_reader >= SLOTS) {
        xSemaphoreGive(lock); /* too late, or way ahead */
        return;
    }
    int16_t gap = (int16_t)(seq - next_seq);
    if (!resent && gap > 0) {
        request_resend(next_seq, gap); /* packets lost on the way */
    }
    if (gap >= 0) {
        next_seq = seq + 1;
    }

    slot_t *slot = &slots[seq % SLOTS];
    slot->seq = seq;
    slot->frames = 0;
    if (codec == CODEC_PCM) {
        size_t samples = len / 2 < FRAMES * 2 ? len / 2 : FRAMES * 2;
        for (size_t i = 0; i < samples; i++) {
            slot->pcm[i] = (int16_t)(payload[2 * i] << 8 | payload[2 * i + 1]);
        }
        slot->frames = samples / 2;
    } else if (alac_dec) {
        esp_audio_dec_in_raw_t raw = { .buffer = payload, .len = len };
        esp_audio_dec_out_frame_t frame = { .buffer = (uint8_t *)slot->pcm, .len = sizeof(slot->pcm) };
        if (esp_audio_dec_process(alac_dec, &raw, &frame) == ESP_AUDIO_ERR_OK) {
            slot->frames = frame.decoded_size / 4;
        }
    }
    slot->ready = slot->frames > 0;
    stats.received += slot->ready;
    xSemaphoreGive(lock);
}

size_t airplay_read(int16_t *pcm, size_t max_frames, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    for (;;) {
        if (!active) {
            return 0;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        int16_t n = ahead();
        if (!started && n >= START_PACKETS) {
            started = true;
        }
        if (started && n > SLOTS * 3 / 4) {
            read_seq++; /* the sender's clock runs faster than ours: drop a packet */
            n--;
        }
        if (started && n > 0) {
            slot_t *slot = &slots[read_seq % SLOTS];
            size_t frames = 0;
            if (slot->ready && slot->seq == read_seq) {
                frames = slot->frames < max_frames ? slot->frames : max_frames;
                memcpy(pcm, slot->pcm, frames * 4);
                slot->ready = false;
                stats.played++;
            } else {
                stats.lost++;
                /* lost even after the resend request: silence instead */
                frames = FRAMES < max_frames ? FRAMES : max_frames;
                memset(pcm, 0, frames * 4);
            }
            read_seq++;
            xSemaphoreGive(lock);
            return frames;
        }
        if (started && n == 0) {
            started = false; /* ran dry (paused, or the network stalled): rebuffer */
        }
        xSemaphoreGive(lock);
        if (esp_timer_get_time() > deadline) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------- UDP: audio, control, timing ---------- */

static int udp_open(uint16_t *port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (s >= 0) {
            close(s);
        }
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(s, (struct sockaddr *)&addr, &len);
    *port = ntohs(addr.sin_port);
    return s;
}


/* NTP-style timing request: senders like to see them, the answer is not needed for playback */
static void send_timing_request(void)
{
    if (timing_sock < 0 || client_timing_port == 0) {
        return;
    }
    uint8_t pkt[32] = { 0x80, 0xD2, 0x00, 0x07 };
    int64_t us = esp_timer_get_time();
    uint32_t sec = us / 1000000 + 0x83AA7E80; /* NTP epoch offset, the value itself is arbitrary */
    uint32_t frac = (uint32_t)((us % 1000000) * 4294.967296);
    for (int i = 0; i < 4; i++) {
        pkt[24 + i] = sec >> (24 - 8 * i);
        pkt[28 + i] = frac >> (24 - 8 * i);
    }
    struct sockaddr_in to = client;
    to.sin_port = htons(client_timing_port);
    sendto(timing_sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
}

static void udp_task(void *arg)
{
    static uint8_t buf[MAX_UDP];
    int64_t last_timing = 0;
    for (;;) {
        if (audio_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        int max_fd = -1;
        int socks[] = { audio_sock, control_sock, timing_sock };
        for (int i = 0; i < 3; i++) {
            if (socks[i] >= 0) {
                FD_SET(socks[i], &fds);
                max_fd = socks[i] > max_fd ? socks[i] : max_fd;
            }
        }
        struct timeval tv = { .tv_usec = 100000 };
        if (select(max_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            if (audio_sock >= 0 && FD_ISSET(audio_sock, &fds)) {
                int n = recv(audio_sock, buf, sizeof(buf), 0);
                if (active && n > 12 && (buf[1] & 0x7F) == 0x60) {
                    store_packet(buf[2] << 8 | buf[3], buf + 12, n - 12, false);
                }
            }
            if (control_sock >= 0 && FD_ISSET(control_sock, &fds)) {
                int n = recv(control_sock, buf, sizeof(buf), 0);
                /* 0x56: a resent audio packet, the original RTP packet follows a 4-byte header */
                if (active && n > 16 && (buf[1] & 0x7F) == 0x56) {
                    store_packet(buf[6] << 8 | buf[7], buf + 16, n - 16, true);
                }
            }
            if (timing_sock >= 0 && FD_ISSET(timing_sock, &fds)) {
                recv(timing_sock, buf, sizeof(buf), 0); /* timing replies: not used */
            }
        }
        int64_t now = esp_timer_get_time();
        if (active && now - last_timing > TIMING_MS * 1000LL) {
            last_timing = now;
            send_timing_request();
        }
    }
}

/* ---------- RTSP ---------- */

typedef struct {
    char method[24];
    char *headers; /* raw header block */
    char *body;
    size_t body_len;
} rtsp_req_t;

static bool header(const rtsp_req_t *r, const char *name, char *out, size_t size)
{
    size_t n = strlen(name);
    for (const char *p = r->headers; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
            p += n + 1;
            while (*p == ' ') {
                p++;
            }
            size_t len = strcspn(p, "\r\n");
            len = len < size - 1 ? len : size - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

static void rtsp_reply(int conn, const rtsp_req_t *r, const char *status, const char *extra, const char *body)
{
    char cseq[16] = "0";
    header(r, "CSeq", cseq, sizeof(cseq));
    char head[1024];
    int n = snprintf(head, sizeof(head),
                     "RTSP/1.0 %s\r\nCSeq: %s\r\nServer: AirTunes/105.1\r\nAudio-Jack-Status: connected; type=analog\r\n%s",
                     status, cseq, extra ? extra : "");
    if (body) {
        n += snprintf(head + n, sizeof(head) - n, "Content-Type: text/parameters\r\nContent-Length: %d\r\n",
                      (int)strlen(body));
    }
    n += snprintf(head + n, sizeof(head) - n, "\r\n");
    send(conn, head, n, 0);
    if (body) {
        send(conn, body, strlen(body), 0);
    }
}

/* Apple-Response: RSA signature over challenge + our IP + MAC, base64 without padding */
static void apple_response(int conn, const char *challenge_b64, char *out, size_t out_size)
{
    uint8_t buf[48] = { 0 };
    size_t len = b64_decode(challenge_b64, buf, 16);
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(conn, (struct sockaddr *)&local, &local_len);
    memcpy(buf + len, &local.sin_addr.s_addr, 4);
    len += 4;
    memcpy(buf + len, mac, 6);
    len += 6;
    len = len < 32 ? 32 : len;

    uint8_t sig[256];
    size_t sig_len = rsa_apply(buf, len, sig, sizeof(sig), true);
    size_t b64_len = 0;
    out[0] = '\0';
    if (sig_len && mbedtls_base64_encode((unsigned char *)out, out_size, &b64_len, sig, sig_len) == 0) {
        out[strcspn(out, "=")] = '\0';
    }
}

/* SDP of ANNOUNCE: codec, ALAC parameters, AES key */
static bool parse_announce(const char *sdp)
{
    const char *rtpmap = strstr(sdp, "a=rtpmap:");
    const char *format = rtpmap ? strchr(rtpmap, ' ') : NULL; /* "a=rtpmap:96 AppleLossless" */
    codec = format && strncasecmp(format + 1, "L16", 3) == 0 ? CODEC_PCM : CODEC_ALAC;

    /* a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100 -> ALACSpecificConfig, big-endian */
    const char *fmtp = strstr(sdp, "a=fmtp:");
    unsigned v[12] = { 0 };
    if (codec == CODEC_ALAC && (!fmtp || sscanf(fmtp, "a=fmtp:%u %u %u %u %u %u %u %u %u %u %u %u",
                                                &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                                                &v[8], &v[9], &v[10], &v[11]) != 12)) {
        ESP_LOGE(TAG, "No ALAC parameters in ANNOUNCE");
        return false;
    }
    uint8_t *c = alac_cookie;
    c[0] = v[1] >> 24; c[1] = v[1] >> 16; c[2] = v[1] >> 8; c[3] = v[1]; /* frameLength */
    c[4] = v[2];                                                         /* compatibleVersion */
    c[5] = v[3];                                                         /* bitDepth */
    c[6] = v[4]; c[7] = v[5]; c[8] = v[6];                               /* pb, mb, kb */
    c[9] = v[7];                                                         /* numChannels */
    c[10] = v[8] >> 8; c[11] = v[8];                                     /* maxRun */
    c[12] = v[9] >> 24; c[13] = v[9] >> 16; c[14] = v[9] >> 8; c[15] = v[9];       /* maxFrameBytes */
    c[16] = v[10] >> 24; c[17] = v[10] >> 16; c[18] = v[10] >> 8; c[19] = v[10];   /* avgBitRate */
    c[20] = v[11] >> 24; c[21] = v[11] >> 16; c[22] = v[11] >> 8; c[23] = v[11];   /* sampleRate */

    const char *key = strstr(sdp, "a=rsaaeskey:");
    const char *iv = strstr(sdp, "a=aesiv:");
    encrypted = key && iv;
    if (encrypted) {
        uint8_t enc[512], aes[256];
        size_t enc_len = b64_decode(key + 12, enc, sizeof(enc));
        if (rsa_apply(enc, enc_len, aes, sizeof(aes), false) != 16 ||
            b64_decode(iv + 8, aes_iv, sizeof(aes_iv)) != 16) {
            ESP_LOGE(TAG, "Bad AES key in ANNOUNCE");
            return false;
        }
        set_aes_key(aes);
    }

    if (alac_dec) {
        esp_audio_dec_close(alac_dec);
        alac_dec = NULL;
    }
    if (codec == CODEC_ALAC) {
        esp_alac_dec_cfg_t alac_cfg = { .codec_spec_info = alac_cookie, .spec_info_len = sizeof(alac_cookie) };
        esp_audio_dec_cfg_t cfg = { .type = ESP_AUDIO_TYPE_ALAC, .cfg = &alac_cfg, .cfg_sz = sizeof(alac_cfg) };
        if (esp_audio_dec_open(&cfg, &alac_dec) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "ALAC decoder failed to open");
            return false;
        }
    }
    ESP_LOGI(TAG, "Stream: %s, %s", codec == CODEC_ALAC ? "ALAC" : "PCM", encrypted ? "encrypted" : "plain");
    return true;
}

/* "volume: -12.5": 0 dB is full, -30 is the bottom of the slider, -144 is mute */
static void parse_volume(const char *body)
{
    const char *p = strstr(body, "volume:");
    if (p) {
        float db = strtof(p + 7, NULL);
        int v = db <= -30.0f ? 0 : db >= 0.0f ? 100 : (int)((db + 30.0f) * 100.0f / 30.0f + 0.5f);
        volume = v;
    }
}

static void session_end(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (active) {
        ESP_LOGI(TAG, "Session ended");
    }
    active = false;
    buffer_reset();
    xSemaphoreGive(lock);
}

/* Handle one request; false closes the connection */
static bool rtsp_handle(int conn, rtsp_req_t *r)
{
    char value[256];
    char extra[768]; /* Apple-Response alone is 344 characters */

    if (strcmp(r->method, "OPTIONS") == 0) {
        int n = snprintf(extra, sizeof(extra), "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                                               "OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
        if (header(r, "Apple-Challenge", value, sizeof(value))) {
            char resp[400];
            apple_response(conn, value, resp, sizeof(resp));
            snprintf(extra + n, sizeof(extra) - n, "Apple-Response: %s\r\n", resp);
        }
        rtsp_reply(conn, r, "200 OK", extra, NULL);
    } else if (strcmp(r->method, "ANNOUNCE") == 0) {
        bool ok = r->body && parse_announce(r->body);
        rtsp_reply(conn, r, ok ? "200 OK" : "400 Bad Request", NULL, NULL);
    } else if (strcmp(r->method, "SETUP") == 0) {
        /* Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=6001;timing_port=6002 */
        header(r, "Transport", value, sizeof(value));
        const char *cp = strstr(value, "control_port=");
        const char *tp = strstr(value, "timing_port=");
        client_control_port = cp ? atoi(cp + 13) : 0;
        client_timing_port = tp ? atoi(tp + 12) : 0;
        socklen_t len = sizeof(client);
        getpeername(conn, (struct sockaddr *)&client, &len);

        if (audio_sock < 0 || control_sock < 0 || timing_sock < 0) {
            rtsp_reply(conn, r, "503 Service Unavailable", NULL, NULL);
            return true;
        }
        snprintf(extra, sizeof(extra), "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;"
                                       "control_port=%u;timing_port=%u\r\nSession: 1\r\n",
                 audio_port, control_port, timing_port);
        rtsp_reply(conn, r, "200 OK", extra, NULL);
    } else if (strcmp(r->method, "RECORD") == 0) {
        xSemaphoreTake(lock, portMAX_DELAY);
        buffer_reset();
        memset(&stats, 0, sizeof(stats));
        active = true;
        xSemaphoreGive(lock);
        ESP_LOGI(TAG, "Streaming from " IPSTR, IP2STR((esp_ip4_addr_t *)&client.sin_addr.s_addr));
        rtsp_reply(conn, r, "200 OK", "Audio-Latency: 11025\r\n", NULL);
    } else if (strcmp(r->method, "FLUSH") == 0) {
        /* Pause or seek on the phone: drop what is buffered */
        xSemaphoreTake(lock, portMAX_DELAY);
        buffer_reset();
        xSemaphoreGive(lock);
        rtsp_reply(conn, r, "200 OK", NULL, NULL);
    } else if (strcmp(r->method, "SET_PARAMETER") == 0) {
        if (r->body && header(r, "Content-Type", value, sizeof(value)) && strstr(value, "text/parameters")) {
            parse_volume(r->body);
        }
        rtsp_reply(conn, r, "200 OK", NULL, NULL);
    } else if (strcmp(r->method, "GET_PARAMETER") == 0) {
        char body[32];
        snprintf(body, sizeof(body), "volume: %.1f\r\n", volume * 30.0f / 100.0f - 30.0f);
        rtsp_reply(conn, r, "200 OK", NULL, body);
    } else if (strcmp(r->method, "TEARDOWN") == 0) {
        rtsp_reply(conn, r, "200 OK", "Connection: close\r\n", NULL);
        return false;
    } else {
        rtsp_reply(conn, r, "501 Not Implemented", NULL, NULL); /* AirPlay 2 pairing etc. */
    }
    return true;
}

/* Read one request into buf; false when the connection is gone */
static bool rtsp_read(int conn, char *buf, rtsp_req_t *r)
{
    size_t len = 0;
    char *end = NULL;
    while ((end = strstr(buf, "\r\n\r\n")) == NULL) {
        if (len >= RTSP_BUF - 1) {
            return false;
        }
        int n = recv(conn, buf + len, RTSP_BUF - 1 - len, 0);
        if (n <= 0) {
            return false;
        }
        len += n;
        buf[len] = '\0';
    }
    *end = '\0';
    char *body_start = end + 4;
    size_t have = len - (body_start - buf);

    sscanf(buf, "%23s", r->method);
    r->headers = strchr(buf, '\n') ? strchr(buf, '\n') + 1 : body_start;
    char cl[16];
    size_t body_len = header(r, "Content-Length", cl, sizeof(cl)) ? (size_t)atoi(cl) : 0;

    /* Body: keep it if it fits, otherwise (cover art) read and drop it */
    size_t room = RTSP_BUF - 1 - (body_start - buf);
    bool keep = body_len <= room && body_len <= RTSP_BODY_MAX;
    size_t got = have < body_len ? have : body_len;
    while (got < body_len) {
        char sink[256];
        char *dst = keep ? body_start + got : sink;
        size_t want = keep ? body_len - got : (body_len - got < sizeof(sink) ? body_len - got : sizeof(sink));
        int n = recv(conn, dst, want, 0);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    if (keep) {
        body_start[body_len] = '\0';
    }
    r->body = keep && body_len ? body_start : NULL;
    r->body_len = body_len;
    return true;
}

static void rtsp_serve(int conn)
{
    char *buf = heap_caps_malloc(RTSP_BUF, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        buf[0] = '\0';
        rtsp_req_t r = { 0 };
        /* Waiting in 1 s steps lets the stop button and the settings switch end a session */
        bool got = false;
        while (!stop_requested) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(conn, &fds);
            struct timeval wait = { .tv_sec = 1 };
            int sel = select(conn + 1, &fds, NULL, NULL, &wait);
            if (sel > 0) {
                got = rtsp_read(conn, buf, &r);
                break;
            }
            if (sel < 0) {
                break;
            }
            cast_settings_t cast;
            settings_get_cast(&cast);
            if (!cast.airplay) {
                break;
            }
        }
        if (!got || !rtsp_handle(conn, &r)) {
            break;
        }
    }
    free(buf);
}

static void advertise(bool on)
{
    static bool advertised;
    if (on == advertised) {
        return;
    }
    if (on) {
        char name[64];
        snprintf(name, sizeof(name), "%02X%02X%02X%02X%02X%02X@" SERVICE_NAME,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mdns_txt_item_t txt[] = {
            { "txtvers", "1" }, { "ch", "2" }, { "cn", "0,1" }, { "et", "0,1" }, { "ek", "1" },
            { "sv", "false" }, { "da", "true" }, { "sr", "44100" }, { "ss", "16" }, { "pw", "false" },
            { "vn", "65537" }, { "tp", "UDP" }, { "md", "0" }, { "vs", "105.1" }, { "sm", "false" },
            { "am", "NewYearStar" },
        };
        if (mdns_service_add(name, "_raop", "_tcp", RTSP_PORT, txt, sizeof(txt) / sizeof(txt[0])) == ESP_OK) {
            ESP_LOGI(TAG, "Announced as \"%s\"", SERVICE_NAME);
        }
    } else {
        mdns_service_remove("_raop", "_tcp");
    }
    advertised = on;
}

static void rtsp_task(void *arg)
{
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(RTSP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        ESP_LOGE(TAG, "RTSP port %d unavailable", RTSP_PORT);
        vTaskDelete(NULL);
    }
    for (;;) {
        cast_settings_t cast;
        settings_get_cast(&cast);
        advertise(cast.airplay);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        struct timeval tv = { .tv_sec = 1 };
        if (select(listener + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        int conn = accept(listener, NULL, NULL);
        if (conn < 0) {
            continue;
        }
        if (!cast.airplay) {
            close(conn);
            continue;
        }
        stop_requested = false;
        rtsp_serve(conn);
        close(conn);
        session_end();
    }
}

/* ---------- public ---------- */

void airplay_start(void)
{
    lock = xSemaphoreCreateMutex();
    slots = heap_caps_calloc(SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    assert(lock && slots);
    psa_crypto_init();
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    audio_sock = udp_open(&audio_port);
    control_sock = udp_open(&control_port);
    timing_sock = udp_open(&timing_port);
    if (audio_sock < 0 || control_sock < 0 || timing_sock < 0) {
        ESP_LOGE(TAG, "No UDP sockets, AirPlay is off");
    }
    /* Stacks in PSRAM: internal RAM is short, and these never touch the flash */
    xTaskCreateWithCaps(rtsp_task, "airplay_rtsp", 8192, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(udp_task, "airplay_udp", 6144, NULL, 6, NULL, MALLOC_CAP_SPIRAM);
}

bool airplay_active(void)
{
    return active;
}

void airplay_get_stats(airplay_stats_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = stats;
    xSemaphoreGive(lock);
}

uint8_t airplay_volume(void)
{
    return volume;
}

void airplay_stop(void)
{
    if (active) {
        stop_requested = true;
    }
}

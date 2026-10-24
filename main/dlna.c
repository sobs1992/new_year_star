#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "dlna.h"
#include "settings.h"

#define FRIENDLY_NAME   "Новогодняя звезда"
#define DESC_PATH       "/dlna/description.xml"
#define SSDP_ADDR       "239.255.255.250"
#define SSDP_PORT       1900
#define SSDP_MAX_AGE    1800
#define SSDP_NOTIFY_MS  60000      /* re-announce, so control points that missed it find us */
#define SOAP_MAX_BODY   16384
#define META_SIZE       4096       /* DIDL-Lite of the current track, kept for GetMediaInfo */
#define MAX_SUBSCRIBERS 6
#define SUB_TIMEOUT_S   1800
#define EVENT_URL_SIZE  192

#define DEVICE_TYPE     "urn:schemas-upnp-org:device:MediaRenderer:1"
#define AVT_TYPE        "urn:schemas-upnp-org:service:AVTransport:1"
#define RCS_TYPE        "urn:schemas-upnp-org:service:RenderingControl:1"
#define CMS_TYPE        "urn:schemas-upnp-org:service:ConnectionManager:1"

/* What the star can decode, as DLNA protocolInfo */
#define SINK_PROTOCOLS \
    "http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,http-get:*:audio/aac:*," \
    "http-get:*:audio/x-aac:*,http-get:*:audio/mp4:*,http-get:*:audio/x-m4a:*," \
    "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,http-get:*:audio/wav:*," \
    "http-get:*:audio/x-wav:*"

static const char *TAG = "dlna";

typedef enum {
    SERVICE_AVT,
    SERVICE_RCS,
    SERVICE_CMS,
    SERVICE_COUNT,
} service_t;

static const char *const SERVICE_NAMES[SERVICE_COUNT] = { "AVTransport", "RenderingControl", "ConnectionManager" };
static const char *const SERVICE_TYPES[SERVICE_COUNT] = { AVT_TYPE, RCS_TYPE, CMS_TYPE };

typedef enum {
    TS_NO_MEDIA,
    TS_STOPPED,
    TS_PLAYING,
    TS_PAUSED,
} transport_state_t;

static const char *const TS_NAMES[] = {
    [TS_NO_MEDIA] = "NO_MEDIA_PRESENT",
    [TS_STOPPED] = "STOPPED",
    [TS_PLAYING] = "PLAYING",
    [TS_PAUSED] = "PAUSED_PLAYBACK",
};

typedef struct {
    bool used;
    bool initial;          /* the first event, with the full state, is not sent yet */
    service_t service;
    char sid[48];
    char url[EVENT_URL_SIZE];
    uint32_t seq;
    int64_t expires_us;
} subscriber_t;

static SemaphoreHandle_t lock;
static char uuid[48];
static transport_state_t state = TS_NO_MEDIA;
static char *uri;          /* STREAM_URL_SIZE */
static char *meta;         /* META_SIZE, unescaped DIDL-Lite */
static char mime[48];
static char title[64];
static uint32_t duration_ms;
static uint32_t position_ms;
static uint32_t generation;
static uint32_t start_ms, byte_offset;  /* where the current generation starts */
static char *next_uri;                  /* STREAM_URL_SIZE, queued by SetNextAVTransportURI */
static char *next_meta;                 /* META_SIZE */
static uint32_t content_length, bitrate; /* reported by the player */
static bool seekable;
static uint8_t volume = 50;
static bool mute;
static subscriber_t subs[MAX_SUBSCRIBERS];
static bool dirty[SERVICE_COUNT]; /* state changed: event to send */
static TaskHandle_t event_task_handle;

/* ---------- small XML helpers ---------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n < b->cap) {
        memcpy(b->buf + b->len, s, n + 1);
        b->len += n;
    }
}

static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len = b->len + n < b->cap ? b->len + n : b->cap - 1;
    }
}

static void sb_escaped(sbuf_t *b, const char *s)
{
    for (; *s && b->len + 6 < b->cap; s++) {
        switch (*s) {
        case '&': sb_add(b, "&amp;"); break;
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '"': sb_add(b, "&quot;"); break;
        default:
            b->buf[b->len++] = *s;
            b->buf[b->len] = '\0';
        }
    }
}

static void xml_unescape(char *s)
{
    static const struct {
        const char *entity;
        char c;
    } ENTITIES[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
    char *out = s;
    while (*s) {
        bool replaced = false;
        for (size_t i = 0; *s == '&' && i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
            size_t n = strlen(ENTITIES[i].entity);
            if (strncmp(s, ENTITIES[i].entity, n) == 0) {
                *out++ = ENTITIES[i].c;
                s += n;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

/* Unescaped text of the first <tag ...>text</tag> (tag may carry a namespace prefix) */
static bool xml_get(const char *xml, const char *tag, char *out, size_t size)
{
    size_t tag_len = strlen(tag);
    for (const char *p = strchr(xml, '<'); p; p = strchr(p + 1, '<')) {
        const char *name = p + 1;
        const char *gt = strchr(name, '>');
        if (gt == NULL) {
            return false;
        }
        const char *colon = strchr(name, ':');
        if (colon && colon < gt && strncmp(colon + 1, tag, tag_len) == 0) {
            name = colon + 1;
        }
        if (strncmp(name, tag, tag_len) != 0 || (name[tag_len] != '>' && name[tag_len] != ' ' &&
                                                  name[tag_len] != '/')) {
            continue;
        }
        if (gt[-1] == '/') {
            out[0] = '\0'; /* <tag/> */
            return true;
        }
        const char *end = strstr(gt + 1, "</");
        if (end == NULL) {
            return false;
        }
        size_t n = end - (gt + 1);
        n = n < size - 1 ? n : size - 1;
        memcpy(out, gt + 1, n);
        out[n] = '\0';
        xml_unescape(out);
        return true;
    }
    return false;
}

/* Value of attr="..." anywhere after the first <tag */
static bool xml_attr(const char *xml, const char *tag, const char *attr, char *out, size_t size)
{
    const char *p = strstr(xml, tag);
    if (p == NULL) {
        return false;
    }
    char key[32];
    snprintf(key, sizeof(key), " %s=\"", attr);
    const char *gt = strchr(p, '>');
    const char *a = strstr(p, key);
    if (a == NULL || (gt && a > gt)) {
        return false;
    }
    a += strlen(key);
    const char *end = strchr(a, '"');
    if (end == NULL) {
        return false;
    }
    size_t n = (size_t)(end - a);
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(out, a, n);
    out[n] = '\0';
    xml_unescape(out);
    return true;
}

static void format_time(uint32_t ms, char *out, size_t size)
{
    uint32_t s = ms / 1000;
    snprintf(out, size, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, s / 3600, s / 60 % 60, s % 60);
}

/* "H:MM:SS[.fff]" */
static uint32_t parse_time(const char *s)
{
    unsigned h = 0, m = 0;
    float sec = 0;
    return sscanf(s, "%u:%u:%f", &h, &m, &sec) == 3 ? (uint32_t)((h * 3600 + m * 60 + sec) * 1000) : 0;
}

static void mark_dirty(service_t s)
{
    dirty[s] = true;
    if (event_task_handle) {
        xTaskNotifyGive(event_task_handle);
    }
}

/* ---------- descriptions ---------- */

static char *description;

static void build_description(void)
{
    static const char *const TEMPLATE =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion>"
        "<device>"
        "<deviceType>" DEVICE_TYPE "</deviceType>"
        "<friendlyName>" FRIENDLY_NAME "</friendlyName>"
        "<manufacturer>DIY</manufacturer>"
        "<modelName>New Year Star</modelName>"
        "<modelNumber>1</modelNumber>"
        "<UDN>%s</UDN>"
        "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
        "<serviceList>"
        "<service><serviceType>" AVT_TYPE "</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<SCPDURL>/dlna/AVTransport.xml</SCPDURL>"
        "<controlURL>/dlna/control/AVTransport</controlURL>"
        "<eventSubURL>/dlna/event/AVTransport</eventSubURL></service>"
        "<service><serviceType>" RCS_TYPE "</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "<SCPDURL>/dlna/RenderingControl.xml</SCPDURL>"
        "<controlURL>/dlna/control/RenderingControl</controlURL>"
        "<eventSubURL>/dlna/event/RenderingControl</eventSubURL></service>"
        "<service><serviceType>" CMS_TYPE "</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
        "<SCPDURL>/dlna/ConnectionManager.xml</SCPDURL>"
        "<controlURL>/dlna/control/ConnectionManager</controlURL>"
        "<eventSubURL>/dlna/event/ConnectionManager</eventSubURL></service>"
        "</serviceList>"
        "</device>"
        "</root>";
    size_t size = strlen(TEMPLATE) + sizeof(uuid);
    description = malloc(size);
    assert(description);
    snprintf(description, size, TEMPLATE, uuid);
}

#define SCPD_HEAD "<?xml version=\"1.0\" encoding=\"utf-8\"?>" \
    "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
#define ACTION(name, args) "<action><name>" name "</name><argumentList>" args "</argumentList></action>"
#define IN(name, var) "<argument><name>" name "</name><direction>in</direction><relatedStateVariable>" var "</relatedStateVariable></argument>"
#define OUT(name, var) "<argument><name>" name "</name><direction>out</direction><relatedStateVariable>" var "</relatedStateVariable></argument>"
#define VAR(name, type) "<stateVariable sendEvents=\"no\"><name>" name "</name><dataType>" type "</dataType></stateVariable>"
#define EVENTED(name, type) "<stateVariable sendEvents=\"yes\"><name>" name "</name><dataType>" type "</dataType></stateVariable>"
#define INSTANCE IN("InstanceID", "A_ARG_TYPE_InstanceID")

static const char AVT_SCPD[] = SCPD_HEAD
    "<actionList>"
    ACTION("SetAVTransportURI", INSTANCE IN("CurrentURI", "AVTransportURI") IN("CurrentURIMetaData", "AVTransportURIMetaData"))
    ACTION("GetMediaInfo", INSTANCE OUT("NrTracks", "NumberOfTracks") OUT("MediaDuration", "CurrentMediaDuration")
           OUT("CurrentURI", "AVTransportURI") OUT("CurrentURIMetaData", "AVTransportURIMetaData")
           OUT("NextURI", "NextAVTransportURI") OUT("NextURIMetaData", "NextAVTransportURIMetaData")
           OUT("PlayMedium", "PlaybackStorageMedium") OUT("RecordMedium", "RecordStorageMedium")
           OUT("WriteStatus", "RecordMediumWriteStatus"))
    ACTION("GetTransportInfo", INSTANCE OUT("CurrentTransportState", "TransportState")
           OUT("CurrentTransportStatus", "TransportStatus") OUT("CurrentSpeed", "TransportPlaySpeed"))
    ACTION("GetPositionInfo", INSTANCE OUT("Track", "CurrentTrack") OUT("TrackDuration", "CurrentTrackDuration")
           OUT("TrackMetaData", "CurrentTrackMetaData") OUT("TrackURI", "CurrentTrackURI")
           OUT("RelTime", "RelativeTimePosition") OUT("AbsTime", "AbsoluteTimePosition")
           OUT("RelCount", "RelativeCounterPosition") OUT("AbsCount", "AbsoluteCounterPosition"))
    ACTION("GetDeviceCapabilities", INSTANCE OUT("PlayMedia", "PossiblePlaybackStorageMedia")
           OUT("RecMedia", "PossibleRecordStorageMedia") OUT("RecQualityModes", "PossibleRecordQualityModes"))
    ACTION("GetTransportSettings", INSTANCE OUT("PlayMode", "CurrentPlayMode") OUT("RecQualityMode", "CurrentRecordQualityMode"))
    ACTION("GetCurrentTransportActions", INSTANCE OUT("Actions", "CurrentTransportActions"))
    ACTION("SetNextAVTransportURI", INSTANCE IN("NextURI", "NextAVTransportURI") IN("NextURIMetaData", "NextAVTransportURIMetaData"))
    ACTION("Stop", INSTANCE)
    ACTION("Play", INSTANCE IN("Speed", "TransportPlaySpeed"))
    ACTION("Pause", INSTANCE)
    ACTION("Seek", INSTANCE IN("Unit", "A_ARG_TYPE_SeekMode") IN("Target", "A_ARG_TYPE_SeekTarget"))
    ACTION("Next", INSTANCE)
    ACTION("Previous", INSTANCE)
    "</actionList><serviceStateTable>"
    VAR("TransportState", "string") VAR("TransportStatus", "string") VAR("PlaybackStorageMedium", "string")
    VAR("RecordStorageMedium", "string") VAR("PossiblePlaybackStorageMedia", "string")
    VAR("PossibleRecordStorageMedia", "string") VAR("CurrentPlayMode", "string") VAR("TransportPlaySpeed", "string")
    VAR("RecordMediumWriteStatus", "string") VAR("CurrentRecordQualityMode", "string")
    VAR("PossibleRecordQualityModes", "string") VAR("NumberOfTracks", "ui4") VAR("CurrentTrack", "ui4")
    VAR("CurrentTrackDuration", "string") VAR("CurrentMediaDuration", "string") VAR("CurrentTrackMetaData", "string")
    VAR("CurrentTrackURI", "string") VAR("AVTransportURI", "string") VAR("AVTransportURIMetaData", "string")
    VAR("NextAVTransportURI", "string") VAR("NextAVTransportURIMetaData", "string")
    VAR("RelativeTimePosition", "string") VAR("AbsoluteTimePosition", "string")
    VAR("RelativeCounterPosition", "i4") VAR("AbsoluteCounterPosition", "i4")
    VAR("CurrentTransportActions", "string") EVENTED("LastChange", "string")
    VAR("A_ARG_TYPE_SeekMode", "string") VAR("A_ARG_TYPE_SeekTarget", "string") VAR("A_ARG_TYPE_InstanceID", "ui4")
    "</serviceStateTable></scpd>";

static const char RCS_SCPD[] = SCPD_HEAD
    "<actionList>"
    ACTION("ListPresets", INSTANCE OUT("CurrentPresetNameList", "PresetNameList"))
    ACTION("SelectPreset", INSTANCE IN("PresetName", "A_ARG_TYPE_PresetName"))
    ACTION("GetMute", INSTANCE IN("Channel", "A_ARG_TYPE_Channel") OUT("CurrentMute", "Mute"))
    ACTION("SetMute", INSTANCE IN("Channel", "A_ARG_TYPE_Channel") IN("DesiredMute", "Mute"))
    ACTION("GetVolume", INSTANCE IN("Channel", "A_ARG_TYPE_Channel") OUT("CurrentVolume", "Volume"))
    ACTION("SetVolume", INSTANCE IN("Channel", "A_ARG_TYPE_Channel") IN("DesiredVolume", "Volume"))
    "</actionList><serviceStateTable>"
    VAR("PresetNameList", "string") VAR("A_ARG_TYPE_PresetName", "string") VAR("Mute", "boolean")
    "<stateVariable sendEvents=\"no\"><name>Volume</name><dataType>ui2</dataType>"
    "<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange></stateVariable>"
    EVENTED("LastChange", "string") VAR("A_ARG_TYPE_Channel", "string") VAR("A_ARG_TYPE_InstanceID", "ui4")
    "</serviceStateTable></scpd>";

static const char CMS_SCPD[] = SCPD_HEAD
    "<actionList>"
    ACTION("GetProtocolInfo", OUT("Source", "SourceProtocolInfo") OUT("Sink", "SinkProtocolInfo"))
    ACTION("GetCurrentConnectionIDs", OUT("ConnectionIDs", "CurrentConnectionIDs"))
    ACTION("GetCurrentConnectionInfo", IN("ConnectionID", "A_ARG_TYPE_ConnectionID")
           OUT("RcsID", "A_ARG_TYPE_RcsID") OUT("AVTransportID", "A_ARG_TYPE_AVTransportID")
           OUT("ProtocolInfo", "A_ARG_TYPE_ProtocolInfo") OUT("PeerConnectionManager", "A_ARG_TYPE_ConnectionManager")
           OUT("PeerConnectionID", "A_ARG_TYPE_ConnectionID") OUT("Direction", "A_ARG_TYPE_Direction")
           OUT("Status", "A_ARG_TYPE_ConnectionStatus"))
    "</actionList><serviceStateTable>"
    EVENTED("SourceProtocolInfo", "string") EVENTED("SinkProtocolInfo", "string") EVENTED("CurrentConnectionIDs", "string")
    VAR("A_ARG_TYPE_ConnectionStatus", "string") VAR("A_ARG_TYPE_ConnectionManager", "string")
    VAR("A_ARG_TYPE_Direction", "string") VAR("A_ARG_TYPE_ProtocolInfo", "string")
    VAR("A_ARG_TYPE_ConnectionID", "i4") VAR("A_ARG_TYPE_AVTransportID", "i4") VAR("A_ARG_TYPE_RcsID", "i4")
    "</serviceStateTable></scpd>";

static esp_err_t description_get(httpd_req_t *req)
{
    const char *body = NULL;
    if (strcmp(req->uri, DESC_PATH) == 0) {
        body = description;
    } else if (strcmp(req->uri, "/dlna/AVTransport.xml") == 0) {
        body = AVT_SCPD;
    } else if (strcmp(req->uri, "/dlna/RenderingControl.xml") == 0) {
        body = RCS_SCPD;
    } else if (strcmp(req->uri, "/dlna/ConnectionManager.xml") == 0) {
        body = CMS_SCPD;
    }
    if (body == NULL) {
        return httpd_resp_send_404(req);
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    return httpd_resp_sendstr(req, body);
}

/* ---------- SOAP control ---------- */

static service_t service_from_uri(const char *uri_path, const char *prefix)
{
    const char *name = uri_path + strlen(prefix);
    for (int s = 0; s < SERVICE_COUNT; s++) {
        if (strcmp(name, SERVICE_NAMES[s]) == 0) {
            return s;
        }
    }
    return SERVICE_COUNT;
}

static esp_err_t soap_error(httpd_req_t *req, int code, const char *text)
{
    char body[512];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault>"
             "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
             "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode>"
             "<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>",
             code, text);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    return httpd_resp_sendstr(req, body);
}

/* Parse title, MIME type and duration out of a DIDL-Lite document */
static void parse_metadata(const char *didl)
{
    title[0] = '\0';
    mime[0] = '\0';
    duration_ms = 0;
    xml_get(didl, "title", title, sizeof(title));
    char info[160];
    if (xml_attr(didl, "<res", "protocolInfo", info, sizeof(info))) {
        /* http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;... */
        char *a = strchr(info, ':');
        char *b = a ? strchr(a + 1, ':') : NULL;
        char *c = b ? strchr(b + 1, ':') : NULL;
        if (b && c) {
            *c = '\0';
            strlcpy(mime, b + 1, sizeof(mime));
        }
    }
    char duration[32];
    if (xml_attr(didl, "<res", "duration", duration, sizeof(duration))) {
        duration_ms = parse_time(duration);
    }
}

/* What the control point may offer right now: it greys out the rest */
static const char *transport_actions(void)
{
    static char actions[64];
    actions[0] = '\0';
    if (state == TS_NO_MEDIA) {
        return actions;
    }
    strlcat(actions, state == TS_PLAYING ? "Pause,Stop,Previous" :
                     state == TS_PAUSED ? "Play,Stop,Previous" : "Play,Previous", sizeof(actions));
    if (next_uri[0]) {
        strlcat(actions, ",Next", sizeof(actions));
    }
    if (seekable && state != TS_STOPPED) {
        strlcat(actions, ",Seek", sizeof(actions));
    }
    return actions;
}

/* Restart the stream (new generation) at a position, keeping play / pause */
static void restart_at(uint32_t ms, uint32_t offset)
{
    generation++;
    start_ms = ms;
    byte_offset = offset;
    position_ms = ms;
    mark_dirty(SERVICE_AVT);
}

/* The queued track becomes the current one */
static void switch_to_next(void)
{
    strlcpy(uri, next_uri, STREAM_URL_SIZE);
    strlcpy(meta, next_meta, META_SIZE);
    next_uri[0] = next_meta[0] = '\0';
    parse_metadata(meta);
    content_length = bitrate = 0;
    seekable = false;
    state = TS_PLAYING;
    restart_at(0, 0);
    ESP_LOGI(TAG, "Next: %s", title[0] ? title : uri);
}

/* Runs under lock; writes the action's out arguments to b or returns a UPnP error code */
static int handle_action(service_t service, const char *action, const char *body, sbuf_t *b)
{
    char arg[64];
    char time[16];

    if (service == SERVICE_AVT) {
        if (strcmp(action, "SetAVTransportURI") == 0) {
            if (!xml_get(body, "CurrentURI", uri, STREAM_URL_SIZE) || !uri[0]) {
                return 402;
            }
            if (!xml_get(body, "CurrentURIMetaData", meta, META_SIZE)) {
                meta[0] = '\0';
            }
            parse_metadata(meta);
            next_uri[0] = next_meta[0] = '\0';
            content_length = bitrate = 0;
            seekable = false;
            position_ms = start_ms = byte_offset = 0;
            if (state == TS_PLAYING) {
                generation++; /* new track while playing: switch to it */
            } else {
                state = TS_STOPPED;
            }
            ESP_LOGI(TAG, "URI: %s (%s, %s)", uri, title[0] ? title : "no title", mime[0] ? mime : "?");
            mark_dirty(SERVICE_AVT);
        } else if (strcmp(action, "Play") == 0) {
            if (state == TS_NO_MEDIA) {
                return 701;
            }
            if (state == TS_STOPPED) {
                restart_at(0, 0);
            }
            state = TS_PLAYING;
            mark_dirty(SERVICE_AVT);
        } else if (strcmp(action, "Pause") == 0) {
            if (state != TS_PLAYING) {
                return 701;
            }
            state = TS_PAUSED;
            mark_dirty(SERVICE_AVT);
        } else if (strcmp(action, "Stop") == 0) {
            if (state != TS_NO_MEDIA) {
                state = TS_STOPPED;
                position_ms = 0;
                mark_dirty(SERVICE_AVT);
            }
        } else if (strcmp(action, "SetNextAVTransportURI") == 0) {
            if (!xml_get(body, "NextURI", next_uri, STREAM_URL_SIZE)) {
                return 402;
            }
            if (!xml_get(body, "NextURIMetaData", next_meta, META_SIZE)) {
                next_meta[0] = '\0';
            }
            ESP_LOGI(TAG, "Queued next: %s", next_uri);
            mark_dirty(SERVICE_AVT);
        } else if (strcmp(action, "Seek") == 0) {
            char unit[16], target[32];
            if (!xml_get(body, "Unit", unit, sizeof(unit)) || !xml_get(body, "Target", target, sizeof(target))) {
                return 402;
            }
            if (strcmp(unit, "REL_TIME") != 0 && strcmp(unit, "ABS_TIME") != 0) {
                return 710; /* track numbers / byte counters */
            }
            if (state != TS_PLAYING && state != TS_PAUSED) {
                return 701;
            }
            if (!seekable || (duration_ms == 0 && bitrate == 0)) {
                return 710; /* the server or the format cannot start mid-file */
            }
            uint32_t ms = parse_time(target);
            if (duration_ms && ms > duration_ms) {
                return 711;
            }
            /* Proportional in the file when the length is known, else from the bitrate */
            uint64_t offset = duration_ms ? (uint64_t)content_length * ms / duration_ms
                                          : (uint64_t)ms * bitrate / 8000;
            if (offset >= content_length) {
                return 711;
            }
            ESP_LOGI(TAG, "Seek to %s (byte %" PRIu32 ")", target, (uint32_t)offset);
            restart_at(ms, (uint32_t)offset);
        } else if (strcmp(action, "Next") == 0) {
            if (!next_uri[0]) {
                return 701;
            }
            switch_to_next();
        } else if (strcmp(action, "Previous") == 0) {
            /* No history here, the phone keeps the queue: start this track over */
            if (state == TS_NO_MEDIA) {
                return 701;
            }
            restart_at(0, 0);
        } else if (strcmp(action, "GetTransportInfo") == 0) {
            sb_printf(b, "<CurrentTransportState>%s</CurrentTransportState>"
                         "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
                      TS_NAMES[state]);
        } else if (strcmp(action, "GetPositionInfo") == 0) {
            format_time(duration_ms, time, sizeof(time));
            sb_printf(b, "<Track>%d</Track><TrackDuration>%s</TrackDuration><TrackMetaData>",
                      state == TS_NO_MEDIA ? 0 : 1, time);
            sb_escaped(b, meta);
            sb_add(b, "</TrackMetaData><TrackURI>");
            sb_escaped(b, uri);
            format_time(position_ms, time, sizeof(time));
            sb_printf(b, "</TrackURI><RelTime>%s</RelTime><AbsTime>%s</AbsTime>"
                         "<RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>", time, time);
        } else if (strcmp(action, "GetMediaInfo") == 0) {
            format_time(duration_ms, time, sizeof(time));
            sb_printf(b, "<NrTracks>%d</NrTracks><MediaDuration>%s</MediaDuration><CurrentURI>",
                      state == TS_NO_MEDIA ? 0 : 1, time);
            sb_escaped(b, uri);
            sb_add(b, "</CurrentURI><CurrentURIMetaData>");
            sb_escaped(b, meta);
            sb_add(b, "</CurrentURIMetaData><NextURI>");
            sb_escaped(b, next_uri);
            sb_add(b, "</NextURI><NextURIMetaData>");
            sb_escaped(b, next_meta);
            sb_add(b, "</NextURIMetaData><PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                      "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
        } else if (strcmp(action, "GetCurrentTransportActions") == 0) {
            sb_printf(b, "<Actions>%s</Actions>", transport_actions());
        } else if (strcmp(action, "GetDeviceCapabilities") == 0) {
            sb_add(b, "<PlayMedia>NETWORK</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                      "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
        } else if (strcmp(action, "GetTransportSettings") == 0) {
            sb_add(b, "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
        } else {
            return 401;
        }
    } else if (service == SERVICE_RCS) {
        if (strcmp(action, "GetVolume") == 0) {
            sb_printf(b, "<CurrentVolume>%d</CurrentVolume>", volume);
        } else if (strcmp(action, "SetVolume") == 0) {
            if (!xml_get(body, "DesiredVolume", arg, sizeof(arg))) {
                return 402;
            }
            int v = atoi(arg);
            volume = v < 0 ? 0 : v > 100 ? 100 : v;
            mark_dirty(SERVICE_RCS);
        } else if (strcmp(action, "GetMute") == 0) {
            sb_printf(b, "<CurrentMute>%d</CurrentMute>", mute);
        } else if (strcmp(action, "SetMute") == 0) {
            if (!xml_get(body, "DesiredMute", arg, sizeof(arg))) {
                return 402;
            }
            mute = strcmp(arg, "1") == 0 || strcasecmp(arg, "true") == 0;
            mark_dirty(SERVICE_RCS);
        } else if (strcmp(action, "ListPresets") == 0) {
            sb_add(b, "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>");
        } else if (strcmp(action, "SelectPreset") != 0) {
            return 401;
        }
    } else {
        if (strcmp(action, "GetProtocolInfo") == 0) {
            sb_add(b, "<Source></Source><Sink>" SINK_PROTOCOLS "</Sink>");
        } else if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
            sb_add(b, "<ConnectionIDs>0</ConnectionIDs>");
        } else if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
            sb_add(b, "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo>"
                      "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                      "<Direction>Input</Direction><Status>OK</Status>");
        } else {
            return 401;
        }
    }
    return 0;
}

static esp_err_t control_post(httpd_req_t *req)
{
    service_t service = service_from_uri(req->uri, "/dlna/control/");
    char soap_action[128];
    if (service == SERVICE_COUNT ||
        httpd_req_get_hdr_value_str(req, "SOAPACTION", soap_action, sizeof(soap_action)) != ESP_OK ||
        req->content_len == 0 || req->content_len > SOAP_MAX_BODY) {
        return soap_error(req, 401, "Invalid Action");
    }
    /* "urn:schemas-upnp-org:service:AVTransport:1#Play" */
    char *action = strchr(soap_action, '#');
    if (action == NULL) {
        return soap_error(req, 401, "Invalid Action");
    }
    action++;
    action[strcspn(action, "\"")] = '\0';

    char *body = malloc(req->content_len + 1);
    size_t got = 0;
    while (body && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    if (body == NULL) {
        return soap_error(req, 501, "Out of memory");
    }
    body[got] = '\0';

    /* Escaped metadata can be 3x the raw one */
    sbuf_t b = { .cap = 1024 + 2 * META_SIZE + 2 * STREAM_URL_SIZE };
    b.buf = malloc(b.cap);
    if (b.buf == NULL) {
        free(body);
        return soap_error(req, 501, "Out of memory");
    }
    b.buf[0] = '\0';
    sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                  "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                  "<u:%sResponse xmlns:u=\"%s\">", action, SERVICE_TYPES[service]);

    xSemaphoreTake(lock, portMAX_DELAY);
    int error = handle_action(service, action, body, &b);
    xSemaphoreGive(lock);
    free(body);

    esp_err_t ret;
    if (error) {
        ESP_LOGW(TAG, "%s#%s: error %d", SERVICE_NAMES[service], action, error);
        ret = soap_error(req, error, error == 701 ? "Transition not available" :
                                     error == 710 ? "Seek mode not supported" :
                                     error == 402 ? "Invalid Args" : "Invalid Action");
    } else {
        sb_printf(&b, "</u:%sResponse></s:Body></s:Envelope>", action);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_set_hdr(req, "EXT", "");
        ret = httpd_resp_send(req, b.buf, b.len);
    }
    free(b.buf);
    return ret;
}

/* ---------- GENA eventing ---------- */

/* Full state of a service, as the LastChange event XML (unescaped) */
static void last_change(service_t service, sbuf_t *b)
{
    char time[16];
    if (service == SERVICE_AVT) {
        format_time(duration_ms, time, sizeof(time));
        sb_printf(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\">"
                     "<TransportState val=\"%s\"/><TransportStatus val=\"OK\"/>"
                     "<CurrentTransportActions val=\"%s\"/><NumberOfTracks val=\"%d\"/>"
                     "<CurrentTrackDuration val=\"%s\"/><CurrentMediaDuration val=\"%s\"/>"
                     "<CurrentPlayMode val=\"NORMAL\"/><TransportPlaySpeed val=\"1\"/><AVTransportURI val=\"",
                  TS_NAMES[state], transport_actions(), state == TS_NO_MEDIA ? 0 : 1, time, time);
        sb_escaped(b, uri);
        sb_add(b, "\"/><CurrentTrackURI val=\"");
        sb_escaped(b, uri);
        sb_add(b, "\"/><NextAVTransportURI val=\"");
        sb_escaped(b, next_uri);
        sb_add(b, "\"/></InstanceID></Event>");
    } else if (service == SERVICE_RCS) {
        sb_printf(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\">"
                     "<Volume channel=\"Master\" val=\"%d\"/><Mute channel=\"Master\" val=\"%d\"/>"
                     "</InstanceID></Event>", volume, mute);
    }
}

static void send_event(subscriber_t *sub)
{
    sbuf_t inner = { .cap = 3 * STREAM_URL_SIZE + 1024 };
    sbuf_t body = { .cap = 2 * inner.cap + 512 };
    inner.buf = malloc(inner.cap);
    body.buf = malloc(body.cap);
    if (inner.buf == NULL || body.buf == NULL) {
        free(inner.buf);
        free(body.buf);
        return;
    }
    inner.buf[0] = body.buf[0] = '\0';

    xSemaphoreTake(lock, portMAX_DELAY);
    service_t service = sub->service;
    char url[EVENT_URL_SIZE], sid[48];
    strlcpy(url, sub->url, sizeof(url));
    strlcpy(sid, sub->sid, sizeof(sid));
    uint32_t seq = sub->seq++;
    sub->initial = false;
    if (service == SERVICE_CMS) {
        sb_add(&body, "<?xml version=\"1.0\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                      "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
                      "<e:property><SinkProtocolInfo>" SINK_PROTOCOLS "</SinkProtocolInfo></e:property>"
                      "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property></e:propertyset>");
    } else {
        last_change(service, &inner);
        sb_add(&body, "<?xml version=\"1.0\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                      "<e:property><LastChange>");
        sb_escaped(&body, inner.buf);
        sb_add(&body, "</LastChange></e:property></e:propertyset>");
    }
    xSemaphoreGive(lock);

    char seq_str[12];
    snprintf(seq_str, sizeof(seq_str), "%" PRIu32, seq);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_NOTIFY,
        .timeout_ms = 2000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
        esp_http_client_set_header(client, "NT", "upnp:event");
        esp_http_client_set_header(client, "NTS", "upnp:propchange");
        esp_http_client_set_header(client, "SID", sid);
        esp_http_client_set_header(client, "SEQ", seq_str);
        esp_http_client_set_post_field(client, body.buf, body.len);
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Event to %s failed: %s", url, esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
    free(inner.buf);
    free(body.buf);
}

static void event_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool changed[SERVICE_COUNT];
        xSemaphoreTake(lock, portMAX_DELAY);
        memcpy(changed, dirty, sizeof(changed));
        memset(dirty, 0, sizeof(dirty));
        xSemaphoreGive(lock);

        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            subscriber_t *sub = &subs[i];
            if (sub->used && now > sub->expires_us) {
                sub->used = false;
            }
            if (sub->used && (sub->initial || changed[sub->service])) {
                send_event(sub);
            }
        }
    }
}

static esp_err_t subscribe(httpd_req_t *req)
{
    service_t service = service_from_uri(req->uri, "/dlna/event/");
    if (service == SERVICE_COUNT) {
        return httpd_resp_send_404(req);
    }
    char sid[48] = "", callback[EVENT_URL_SIZE + 8] = "";
    httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid));
    httpd_req_get_hdr_value_str(req, "CALLBACK", callback, sizeof(callback));

    xSemaphoreTake(lock, portMAX_DELAY);
    subscriber_t *sub = NULL;
    for (int i = 0; i < MAX_SUBSCRIBERS && sid[0]; i++) {
        if (subs[i].used && strcmp(subs[i].sid, sid) == 0) {
            sub = &subs[i]; /* renewal */
        }
    }
    if (sub == NULL && sid[0]) {
        xSemaphoreGive(lock);
        httpd_resp_set_status(req, "412 Precondition Failed");
        return httpd_resp_send(req, NULL, 0);
    }
    if (sub == NULL) {
        /* New: CALLBACK is "<http://host:port/path>", maybe several; take the first */
        char *start = strchr(callback, '<');
        char *end = start ? strchr(start, '>') : NULL;
        if (end == NULL) {
            xSemaphoreGive(lock);
            httpd_resp_set_status(req, "412 Precondition Failed");
            return httpd_resp_send(req, NULL, 0);
        }
        *end = '\0';
        /* Reuse the oldest slot when full */
        sub = &subs[0];
        for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
            if (!subs[i].used || subs[i].expires_us < sub->expires_us) {
                sub = &subs[i];
                if (!subs[i].used) {
                    break;
                }
            }
        }
        *sub = (subscriber_t){ .used = true, .initial = true, .service = service };
        strlcpy(sub->url, start + 1, sizeof(sub->url));
        snprintf(sub->sid, sizeof(sub->sid), "uuid:%08" PRIx32 "-%04" PRIx32 "-4000-8000-%012" PRIx32,
                 esp_random(), esp_random() & 0xFFFF, esp_random());
    }
    sub->expires_us = esp_timer_get_time() + SUB_TIMEOUT_S * 1000000LL;
    strlcpy(sid, sub->sid, sizeof(sid));
    xSemaphoreGive(lock);

    httpd_resp_set_hdr(req, "SID", sid);
    httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
    esp_err_t ret = httpd_resp_send(req, NULL, 0);
    xTaskNotifyGive(event_task_handle); /* the initial event goes after the answer */
    return ret;
}

static esp_err_t unsubscribe(httpd_req_t *req)
{
    char sid[48] = "";
    httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid));
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subs[i].used && strcmp(subs[i].sid, sid) == 0) {
            subs[i].used = false;
        }
    }
    xSemaphoreGive(lock);
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- SSDP ---------- */

typedef struct {
    const char *key;   /* WIFI_AP_DEF / WIFI_STA_DEF */
    esp_netif_ip_info_t ip;
    bool joined;
} iface_t;

static iface_t ifaces[] = { { .key = "WIFI_STA_DEF" }, { .key = "WIFI_AP_DEF" } };

/* NT / ST values we answer for and their USN */
static int ssdp_target(int i, char *nt, size_t nt_size, char *usn, size_t usn_size)
{
    static const char *const TYPES[] = { "upnp:rootdevice", NULL, DEVICE_TYPE, AVT_TYPE, RCS_TYPE, CMS_TYPE };
    const int count = sizeof(TYPES) / sizeof(TYPES[0]);
    if (i >= count) {
        return 0;
    }
    if (TYPES[i] == NULL) {
        strlcpy(nt, uuid, nt_size);
        strlcpy(usn, uuid, usn_size);
    } else {
        strlcpy(nt, TYPES[i], nt_size);
        snprintf(usn, usn_size, "%s::%s", uuid, TYPES[i]);
    }
    return count;
}

static void ssdp_location(const esp_netif_ip_info_t *ip, char *out, size_t size)
{
    snprintf(out, size, "http://" IPSTR ":80" DESC_PATH, IP2STR(&ip->ip));
}

static void ssdp_notify(int sock, iface_t *iface, const char *nts)
{
    struct in_addr if_addr = { .s_addr = iface->ip.ip.addr };
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr));
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    inet_aton(SSDP_ADDR, &to.sin_addr);

    char location[64], nt[96], usn[160], msg[512];
    ssdp_location(&iface->ip, location, sizeof(location));
    for (int i = 0; i < ssdp_target(i, nt, sizeof(nt), usn, sizeof(usn)); i++) {
        int n = snprintf(msg, sizeof(msg),
                         "NOTIFY * HTTP/1.1\r\nHOST: " SSDP_ADDR ":1900\r\nCACHE-CONTROL: max-age=%d\r\n"
                         "LOCATION: %s\r\nNT: %s\r\nNTS: %s\r\nSERVER: ESP-IDF UPnP/1.0 NewYearStar/1.0\r\n"
                         "USN: %s\r\n\r\n", SSDP_MAX_AGE, location, nt, nts, usn);
        sendto(sock, msg, n, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void ssdp_answer(int sock, const char *request, const struct sockaddr_in *from)
{
    /* M-SEARCH with an ST we serve: answer from the interface on the asker's subnet */
    char st[128] = "";
    const char *p = strcasestr(request, "\nST:");
    if (strncmp(request, "M-SEARCH", 8) != 0 || p == NULL) {
        return;
    }
    p += 4;
    while (*p == ' ') {
        p++;
    }
    strlcpy(st, p, sizeof(st));
    st[strcspn(st, "\r\n")] = '\0';

    iface_t *iface = NULL;
    for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        if (ifaces[i].joined &&
            (from->sin_addr.s_addr & ifaces[i].ip.netmask.addr) == (ifaces[i].ip.ip.addr & ifaces[i].ip.netmask.addr)) {
            iface = &ifaces[i];
        }
    }
    if (iface == NULL) {
        return;
    }

    char location[64], nt[96], usn[160], msg[512];
    ssdp_location(&iface->ip, location, sizeof(location));
    bool all = strcmp(st, "ssdp:all") == 0;
    for (int i = 0; i < ssdp_target(i, nt, sizeof(nt), usn, sizeof(usn)); i++) {
        if (!all && strcmp(st, nt) != 0) {
            continue;
        }
        int n = snprintf(msg, sizeof(msg),
                         "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=%d\r\nEXT:\r\nLOCATION: %s\r\n"
                         "SERVER: ESP-IDF UPnP/1.0 NewYearStar/1.0\r\nST: %s\r\nUSN: %s\r\n\r\n",
                         SSDP_MAX_AGE, location, nt, usn);
        sendto(sock, msg, n, 0, (const struct sockaddr *)from, sizeof(*from));
    }
}

/* Join the SSDP group on interfaces that got an address (the home network comes and goes) */
static void ssdp_update_interfaces(int sock, bool enabled)
{
    for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        iface_t *iface = &ifaces[i];
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(iface->key);
        if (netif) {
            esp_netif_get_ip_info(netif, &ip);
        }
        bool want = enabled && ip.ip.addr != 0;
        if (iface->joined && (!want || ip.ip.addr != iface->ip.ip.addr)) {
            ssdp_notify(sock, iface, "ssdp:byebye");
            struct ip_mreq mreq = { .imr_interface.s_addr = iface->ip.ip.addr };
            inet_aton(SSDP_ADDR, &mreq.imr_multiaddr);
            setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            iface->joined = false;
        }
        if (want && !iface->joined) {
            iface->ip = ip;
            struct ip_mreq mreq = { .imr_interface.s_addr = ip.ip.addr };
            inet_aton(SSDP_ADDR, &mreq.imr_multiaddr);
            if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0) {
                iface->joined = true;
                ESP_LOGI(TAG, "Announced on " IPSTR, IP2STR(&ip.ip));
                ssdp_notify(sock, iface, "ssdp:alive");
            }
        }
    }
}

static void ssdp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    uint8_t ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "SSDP bind failed");
        vTaskDelete(NULL);
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t last_notify = 0;
    static char buf[1024];
    for (;;) {
        cast_settings_t cast;
        settings_get_cast(&cast);
        if (!cast.dlna) {
            dlna_stop();
        }
        ssdp_update_interfaces(sock, cast.dlna);

        int64_t now = esp_timer_get_time();
        if (cast.dlna && now - last_notify > SSDP_NOTIFY_MS * 1000LL) {
            last_notify = now;
            for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
                if (ifaces[i].joined) {
                    ssdp_notify(sock, &ifaces[i], "ssdp:alive");
                }
            }
        }

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (n > 0 && cast.dlna) {
            buf[n] = '\0';
            ssdp_answer(sock, buf, &from);
        }
    }
}

/* ---------- public ---------- */

void dlna_start(httpd_handle_t httpd)
{
    lock = xSemaphoreCreateMutex();
    uri = heap_caps_calloc(1, STREAM_URL_SIZE, MALLOC_CAP_SPIRAM);
    meta = heap_caps_calloc(1, META_SIZE, MALLOC_CAP_SPIRAM);
    next_uri = heap_caps_calloc(1, STREAM_URL_SIZE, MALLOC_CAP_SPIRAM);
    next_meta = heap_caps_calloc(1, META_SIZE, MALLOC_CAP_SPIRAM);
    assert(lock && uri && meta && next_uri && next_meta);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(uuid, sizeof(uuid), "uuid:4e595374-6172-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    build_description();

    static const httpd_uri_t uris[] = {
        { .uri = "/dlna/control/*", .method = HTTP_POST, .handler = control_post },
        { .uri = "/dlna/event/*", .method = HTTP_SUBSCRIBE, .handler = subscribe },
        { .uri = "/dlna/event/*", .method = HTTP_UNSUBSCRIBE, .handler = unsubscribe },
        { .uri = "/dlna/*", .method = HTTP_GET, .handler = description_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(httpd, &uris[i]));
    }

    /* Stacks in PSRAM: internal RAM is short, and these never touch the flash */
    xTaskCreateWithCaps(event_task, "dlna_events", 6144, NULL, 3, &event_task_handle, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(ssdp_task, "ssdp", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Renderer \"%s\" %s", FRIENDLY_NAME, uuid);
}

bool dlna_get_job(dlna_job_t *out)
{
    if (lock == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool active = state == TS_PLAYING || state == TS_PAUSED;
    if (active && out) {
        out->generation = generation;
        out->paused = state == TS_PAUSED;
        out->start_ms = start_ms;
        out->byte_offset = byte_offset;
        strlcpy(out->url, uri, sizeof(out->url));
        strlcpy(out->mime, mime, sizeof(out->mime));
        strlcpy(out->title, title, sizeof(out->title));
    }
    xSemaphoreGive(lock);
    return active;
}

void dlna_set_position(uint32_t gen, uint32_t ms)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (gen == generation) {
        position_ms = ms;
    }
    xSemaphoreGive(lock);
}

void dlna_set_stream_info(uint32_t gen, uint32_t length, uint32_t bps, bool can_seek)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (gen == generation) {
        if (length) {
            content_length = length;
        }
        if (bps) {
            bitrate = bps;
        }
        if (can_seek != seekable) {
            seekable = can_seek;
            mark_dirty(SERVICE_AVT); /* Seek appears in / leaves the transport actions */
        }
    }
    xSemaphoreGive(lock);
}

void dlna_finished(uint32_t gen, bool failed)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (gen == generation && !failed && next_uri[0] && state == TS_PLAYING) {
        switch_to_next(); /* gapless-ish: the phone queued it with SetNextAVTransportURI */
        xSemaphoreGive(lock);
        return;
    }
    if (gen == generation && (state == TS_PLAYING || state == TS_PAUSED)) {
        ESP_LOGI(TAG, "%s %s", title[0] ? title : uri, failed ? "could not be played" : "finished");
        state = TS_STOPPED;
        position_ms = 0;
        mark_dirty(SERVICE_AVT);
    }
    xSemaphoreGive(lock);
}

void dlna_stop(void)
{
    if (lock == NULL) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state == TS_PLAYING || state == TS_PAUSED) {
        state = TS_STOPPED;
        position_ms = 0;
        mark_dirty(SERVICE_AVT);
    }
    xSemaphoreGive(lock);
}

uint8_t dlna_volume(void)
{
    return volume;
}

bool dlna_muted(void)
{
    return mute;
}

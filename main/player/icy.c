#include <ctype.h>
#include <string.h>
#include "icy.h"

#define TITLE_KEY "StreamTitle='"

void icy_init(icy_t *icy, uint32_t metaint)
{
    memset(icy, 0, sizeof(*icy));
    icy->metaint = metaint;
    icy->audio_left = metaint;
}

size_t icy_strip(icy_t *icy, uint8_t *buf, size_t len, icy_title_fn on_title)
{
    if (icy->metaint == 0) {
        return len;
    }
    size_t audio = 0;
    for (size_t i = 0; i < len;) {
        if (icy->audio_left > 0) {
            size_t n = len - i < icy->audio_left ? len - i : icy->audio_left;
            memmove(buf + audio, buf + i, n);
            audio += n;
            i += n;
            icy->audio_left -= n;
        } else if (icy->meta_left == 0) {
            icy->meta_left = buf[i++] * 16u;
            icy->meta_len = 0;
            if (icy->meta_left == 0) {
                icy->audio_left = icy->metaint;
            }
        } else {
            size_t n = len - i < icy->meta_left ? len - i : icy->meta_left;
            size_t room = sizeof(icy->meta) - 1 - icy->meta_len;
            size_t keep = n < room ? n : room;
            memcpy(icy->meta + icy->meta_len, buf + i, keep);
            icy->meta_len += keep;
            i += n;
            icy->meta_left -= n;
            if (icy->meta_left == 0) {
                icy->meta[icy->meta_len] = '\0';
                if (icy_parse_title(icy->meta, icy->title)) {
                    on_title(icy->title, audio);
                }
                icy->audio_left = icy->metaint;
            }
        }
    }
    return audio;
}

/* Bytes of the UTF-8 sequence a lead byte starts, 0 for a byte that cannot start one */
static size_t utf8_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
}

/* A sequence cut off at the end counts as valid: the block may be longer than what is kept */
static bool valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t len = utf8_len(s[i]);
        if (len == 0) {
            return false;
        }
        for (size_t k = 1; k < len && i + k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

/* Windows-1251 byte to a code point: the Cyrillic letters, dashes, '?' for the rest */
static unsigned cp1251(unsigned char c)
{
    return c < 0x80 ? c : c >= 0xC0 ? 0x410 + (c - 0xC0) : c == 0xA8 ? 0x401 : c == 0xB8 ? 0x451 :
           c == 0x96 || c == 0x97 ? '-' : '?';
}

static void to_utf8(const unsigned char *s, size_t n, char *out, size_t size)
{
    size_t o = 0;
    if (valid_utf8(s, n)) {
        for (size_t i = 0; i < n;) {
            size_t len = utf8_len(s[i]);
            if (i + len > n || o + len >= size) {
                break;
            }
            memcpy(out + o, s + i, len);
            o += len;
            i += len;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            unsigned cp = cp1251(s[i]);
            if (o + (cp < 0x80 ? 1 : 2) >= size) {
                break;
            }
            if (cp < 0x80) {
                out[o++] = (char)cp;
            } else {
                out[o++] = (char)(0xC0 | cp >> 6);
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    }
    out[o] = '\0';
}

bool icy_parse_title(const char *meta, char out[ICY_TITLE_SIZE])
{
    const char *start = strstr(meta, TITLE_KEY);
    if (start == NULL) {
        return false;
    }
    start += strlen(TITLE_KEY);
    /* The value ends at "';": song names have apostrophes of their own */
    const char *end = strstr(start, "';");
    if (end == NULL) {
        /* The last field without ';', or a block longer than what is kept */
        end = start + strlen(start);
        if (end > start && end[-1] == '\'') {
            end--;
        }
    }
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    to_utf8((const unsigned char *)start, end - start, out, ICY_TITLE_SIZE);
    if (strcmp(out, "-") == 0) {
        out[0] = '\0'; /* " - ": no artist, no song (ads, jingles) */
    }
    return true;
}

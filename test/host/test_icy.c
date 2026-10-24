/* icy.c: taking the ICY metadata out of a radio stream, the song title in it */

#include <stdint.h>
#include "icy.h"
#include "test.h"

/* A stream as a server sends it, and the audio in it */
static uint8_t stream[16384];
static size_t stream_len;
static uint8_t audio[4096];
static size_t audio_len;

static void add_audio(size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)(audio_len * 7 + 1);
        stream[stream_len++] = b;
        audio[audio_len++] = b;
    }
}

/* A block of whole 16-byte units, padded with zeros; NULL: the length byte 0 */
static void add_block(const char *text, size_t units)
{
    size_t n = text ? strlen(text) : 0;
    if (units == 0) {
        units = (n + 15) / 16;
    }
    stream[stream_len++] = (uint8_t)units;
    memset(stream + stream_len, 0, units * 16);
    if (text) {
        memcpy(stream + stream_len, text, n);
    }
    stream_len += units * 16;
}

static struct {
    char text[ICY_TITLE_SIZE];
    size_t at; /* audio position the title starts at */
} got[8];
static size_t got_count;
static size_t fed; /* audio bytes icy_strip() returned so far */

static void on_title(const char *title, size_t audio_before)
{
    CHECK(got_count < sizeof(got) / sizeof(got[0]));
    snprintf(got[got_count].text, sizeof(got[0].text), "%s", title);
    got[got_count].at = fed + audio_before;
    got_count++;
}

/* Feeds the stream in chunks of the given size, returns the audio that came out */
static size_t feed(icy_t *icy, uint32_t metaint, size_t chunk, uint8_t *out)
{
    icy_init(icy, metaint);
    got_count = 0;
    fed = 0;
    for (size_t pos = 0; pos < stream_len; pos += chunk) {
        uint8_t buf[sizeof(stream)];
        size_t n = stream_len - pos < chunk ? stream_len - pos : chunk;
        memcpy(buf, stream + pos, n);
        size_t a = icy_strip(icy, buf, n, on_title);
        CHECK(a <= n);
        memcpy(out + fed, buf, a);
        fed += a;
    }
    return fed;
}

static void strips_blocks_at_any_chunk_size(void)
{
    add_audio(10);
    add_block("StreamTitle='Song A';", 0);
    add_audio(10);
    add_block(NULL, 0); /* no change */
    add_audio(10);
    add_block("StreamTitle='Song B';StreamUrl='http://x';", 0);
    add_audio(5);

    static icy_t icy;
    for (size_t chunk = 1; chunk <= stream_len; chunk++) {
        uint8_t out[sizeof(stream)];
        CHECK_EQ_INT(feed(&icy, 10, chunk, out), audio_len);
        CHECK(memcmp(out, audio, audio_len) == 0);
        CHECK_EQ_INT(got_count, 2);
        CHECK_EQ_STR(got[0].text, "Song A");
        CHECK_EQ_INT(got[0].at, 10);
        CHECK_EQ_STR(got[1].text, "Song B");
        CHECK_EQ_INT(got[1].at, 30);
    }
}

static void without_metaint_everything_is_audio(void)
{
    add_audio(100);
    static icy_t icy;
    uint8_t out[sizeof(stream)];
    CHECK_EQ_INT(feed(&icy, 0, 7, out), 100);
    CHECK(memcmp(out, audio, 100) == 0);
    CHECK_EQ_INT(got_count, 0);
}

/* The longest block, 255 units: only its start is kept, the audio after it is intact */
static void longest_block(void)
{
    add_audio(1000);
    add_block("StreamTitle='Long block';StreamUrl='http://x';", 255);
    add_audio(100);
    static icy_t icy;
    uint8_t out[sizeof(stream)];
    CHECK_EQ_INT(feed(&icy, 1000, 4096, out), audio_len);
    CHECK(memcmp(out, audio, audio_len) == 0);
    CHECK_EQ_INT(got_count, 1);
    CHECK_EQ_STR(got[0].text, "Long block");
}

static void titles(void)
{
    char t[ICY_TITLE_SIZE];
    CHECK(icy_parse_title("StreamTitle='Roxette - The Look';", t));
    CHECK_EQ_STR(t, "Roxette - The Look");
    /* An apostrophe in the name, spaces around it (181.fm), more fields after it */
    CHECK(icy_parse_title("StreamTitle=' Can't Stop ';StreamUrl='';StreamArtwork='';", t));
    CHECK_EQ_STR(t, "Can't Stop");
    /* The last field without ';' */
    CHECK(icy_parse_title("StreamTitle='Last'", t));
    CHECK_EQ_STR(t, "Last");
    /* A jingle: empty, or the separator alone */
    CHECK(icy_parse_title("StreamTitle='';", t));
    CHECK_EQ_STR(t, "");
    CHECK(icy_parse_title("StreamTitle=' - ';", t));
    CHECK_EQ_STR(t, "");
    /* No title in the block: the one shown stays */
    CHECK(!icy_parse_title("StreamUrl='http://x';", t));
}

static void utf8_is_kept(void)
{
    char t[ICY_TITLE_SIZE];
    CHECK(icy_parse_title("StreamTitle='Ундервуд - Понте Веккьо';", t));
    CHECK_EQ_STR(t, "Ундервуд - Понте Веккьо");
}

static void long_title_is_cut_at_a_character(void)
{
    char meta[512] = "StreamTitle='";
    for (int i = 0; i < 150; i++) {
        strcat(meta, "я"); /* 2 bytes each, 300 bytes */
    }
    strcat(meta, "';");
    char t[ICY_TITLE_SIZE];
    CHECK(icy_parse_title(meta, t));
    CHECK_EQ_INT(strlen(t), (ICY_TITLE_SIZE - 1) / 2 * 2);
    CHECK(strncmp(t, meta + strlen("StreamTitle='"), strlen(t)) == 0);
}

/* Cut by the block buffer in the middle of a character: still UTF-8, the half is dropped */
static void utf8_cut_off_at_the_end(void)
{
    char t[ICY_TITLE_SIZE];
    CHECK(icy_parse_title("StreamTitle='Ёлка\xd0", t));
    CHECK_EQ_STR(t, "Ёлка");
}

static void windows_1251_is_converted(void)
{
    char t[ICY_TITLE_SIZE];
    /* "Кино – Ёлочка" in Windows-1251 */
    CHECK(icy_parse_title("StreamTitle='\xca\xe8\xed\xee \x96 \xa8\xeb\xee\xf7\xea\xe0';", t));
    CHECK_EQ_STR(t, "Кино - Ёлочка");
}

int main(void)
{
    static const test_t tests[] = {
        TEST(strips_blocks_at_any_chunk_size),
        TEST(without_metaint_everything_is_audio),
        TEST(longest_block),
        TEST(titles),
        TEST(utf8_is_kept),
        TEST(long_title_is_cut_at_a_character),
        TEST(utf8_cut_off_at_the_end),
        TEST(windows_1251_is_converted),
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}

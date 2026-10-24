/* ESP-IDF port: esp_audio_codec links its own Tremor; ours gets a prefix so the two
 * do not clash. The ov_* API, used by TrackPlayer, is renamed through ivorbisfile.h. */
#pragma once
#define ov_bitrate cspot_ov_bitrate
#define ov_bitrate_instant cspot_ov_bitrate_instant
#define ov_clear cspot_ov_clear
#define ov_comment cspot_ov_comment
#define ov_info cspot_ov_info
#define ov_open cspot_ov_open
#define ov_open_callbacks cspot_ov_open_callbacks
#define ov_pcm_seek cspot_ov_pcm_seek
#define ov_pcm_seek_page cspot_ov_pcm_seek_page
#define ov_pcm_tell cspot_ov_pcm_tell
#define ov_pcm_total cspot_ov_pcm_total
#define ov_raw_seek cspot_ov_raw_seek
#define ov_raw_tell cspot_ov_raw_tell
#define ov_raw_total cspot_ov_raw_total
#define ov_read cspot_ov_read
#define ov_seekable cspot_ov_seekable
#define ov_serialnumber cspot_ov_serialnumber
#define ov_streams cspot_ov_streams
#define ov_test cspot_ov_test
#define ov_test_callbacks cspot_ov_test_callbacks
#define ov_test_open cspot_ov_test_open
#define ov_time_seek cspot_ov_time_seek
#define ov_time_seek_page cspot_ov_time_seek_page
#define ov_time_tell cspot_ov_time_tell
#define ov_time_total cspot_ov_time_total

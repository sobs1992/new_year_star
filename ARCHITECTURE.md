# New Year star firmware

An ESP32-C5 (Seeed XIAO ESP32-C5, 8 MB flash, 8 MB PSRAM) inside a Christmas star. It plays
MP3 files from its flash, internet radio, or whatever a phone casts (DLNA, AirPlay 1, Spotify
Connect) on a MAX98357 I2S amplifier; a WS2812 strip follows the music; a 24 GHz mmWave radar
can start the music when someone comes by. Everything is set up on a web page, served on the
star's own Wi-Fi network (always on, captive portal) and on the home network (http://star.local).

## Build, flash, test

```sh
. ./set_env.sh            # ESP-IDF 6.1 environment and the board's serial port
idf.py build
idf.py app-flash          # firmware only: settings (NVS) and uploaded tracks stay
idf.py storage-flash      # the bundled tracks in assets/tracks: overwrites uploaded ones
make -C test/host         # host tests, plain gcc (run idf.py reconfigure once for cJSON)
```

Partitions (`partitions.csv`): NVS, the app (2.5 MB, ~13 % free) and LittleFS with the tracks
(the rest of the flash).

## Modules

All application code is in `main/`; `components/` holds the vendored cspot (Spotify) and the
captive portal DNS server. `app_main()` in `new_year_star.c` starts the modules in order.

| Module | What it does |
|---|---|
| `storage` | LittleFS at `/tracks`: mounting, paths, free space, what a track file is |
| `settings` | All user settings: one JSON document in NVS, validated, versioned |
| `tracks` | Uploading and deleting track files (file + entry in the settings) |
| `player` | Decides what plays and plays it: decoders, volume, I2S output |
| `radio` | HTTP(S) download of a stream or file into a PSRAM ring (radio and DLNA) |
| `led_music` | The LED strip: light music from the audio levels the player feeds it |
| `sensor` | The radar on UART: target, distance, pushing gate / threshold settings |
| `wifi` | Access point, home network, mDNS (`star.local`), scanning |
| `web` | HTTP server: the pages (`assets/*.html`, embedded) and the JSON API |
| `dlna` | UPnP media renderer: SSDP, SOAP control, GENA events |
| `airplay` | AirPlay 1 (RAOP) receiver: RTSP, RTP, ALAC, jitter buffer |
| `spotify` | Spotify Connect over cspot: zeroconf login, decoded audio ring |
| `status` | The yellow LED on the board: blinks the code of the most basic error |
| `button` | BOOT held 5 s: factory reset; polled by the status task |
| `sysinfo` | `/system` page data: CPU per task, RAM, PSRAM, temperature |
| `board.h` | Which GPIO is wired to what |
| `app_config.h` | Names several modules share: the device name, the NVS namespace |

## What plays

`wanted_source()` in `player.c` is the one place that decides who gets the speaker:

1. A phone casting, by priority: AirPlay, Spotify, DLNA (an open DLNA session also holds the
   speaker between the phone's tracks). The receivers are the `CASTS[]` table.
2. After the stop button: nothing. The button also ends the casts of the moment; a cast started
   afterwards plays all the same.
3. The player mode: the playlist (enabled tracks with repeats and pauses) or the radio.

The sensor decides *when* the own mode starts (`wait_for_trigger()`): with it enabled, one
playlist pass or a set number of radio minutes per trigger, then a cooldown. Every playback
loop asks `wanted_source()` before each frame and stops once its source is not wanted, so
settings, the stop button and casts take effect within a frame.

```
MP3 file in LittleFS ──────────────┐
radio / DLNA URL → radio.c ring ───┴─ decode() ─┐
AirPlay (RTP, jitter buffer) ───────────────────┼─ play_pcm(): volume, levels → led_music
Spotify (cspot decoder, ring) ──────────────────┘        └─ I2S → MAX98357
```

`play_pcm()` also measures the loudness of the whole signal and of bass / mids / treble per
block; `led_music` turns them into brightness and color 100 times a second (auto gain,
attack / release). The strip is driven over SPI with DMA: the C5's RMT has no DMA, and Wi-Fi
interrupts corrupt its WS2812 timing.

## Settings

One JSON document in NVS (namespace `star`, key `settings`); Wi-Fi keeps its own keys in the
same namespace. `GET /api/settings` gives the page the document plus value limits, presets and
storage figures; the page posts the whole document back, with the `revision` it edited: a page
holding outdated settings gets 409 instead of overwriting newer ones.

Every field is one line in a table in `settings.c`: JSON key, struct member, range, default,
and `.optional` for fields that came later (saved settings without them still load). The tables
drive the defaults, the validation, the JSON both ways and the limits the page builds its
controls from. Light and sensor settings exist per player mode.

Modules read settings through getters. Most re-read them every frame or poll; the LED task and
the radar re-read only when `settings_led_version()` / `settings_radar_version()` change.

## Tasks and memory

Internal RAM, not PSRAM, is what runs out: the I2S DMA buffers and Wi-Fi need it, and with a
few KB less the I2S channel cannot be set up. While the radio plays about 47 KB of internal RAM
are free (`heap.internal_free` in `/api/status`, `/system` has the details). So:

- Big buffers go to PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`); a plain `malloc()`
  below 4 KB stays internal (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`), C++ `new` of cspot goes
  to PSRAM.
- Task stacks go to PSRAM (`xTaskCreateWithCaps`) unless the task uses the flash driver (NVS,
  LittleFS, reading or writing): a PSRAM stack cannot be used while the flash is busy.
- After a change that adds statics, tasks or allocations: flash and compare `/api/status`.

| Task | Prio | Stack | Stack in | Job |
|---|---|---|---|---|
| `radio` | 6 | 8 KB | PSRAM | HTTP(S) stream into the ring |
| `airplay_udp` | 6 | 6 KB | PSRAM | RTP audio, resend requests, timing |
| `player` | 5 | 8 KB | internal | decoding, I2S output; reads LittleFS |
| `sensor` | 5 | 4 KB | internal | radar UART |
| `httpd` | 5 | 6 KB | internal | web page, API, DLNA SOAP, Spotify zeroconf; writes NVS and LittleFS |
| `dns_server` | 5 | 4 KB | internal | captive portal |
| `led_music` | 4 | 4 KB | internal | LED strip, 10 ms frames |
| `airplay_rtsp` | 4 | 8 KB | PSRAM | AirPlay session |
| `status` | 3 | 3 KB | internal | yellow LED and BOOT button; the factory reset erases NVS |
| `dlna_events`, `ssdp` | 3 | 6 / 4 KB | PSRAM | DLNA events, discovery |
| `spotify` | 3 | 16 KB | PSRAM | Spotify session |
| `cspot_player`, `CSpotTrackQueue`, `mercury_dispatcher` | 3 | 48 / 32 / 10 KB | PSRAM | cspot |

Plus ESP-IDF's own: Wi-Fi, lwIP, mDNS, esp_timer, idle.

## HTTP API

| Request | |
|---|---|
| `GET /`, `GET /system` | the settings page, the system page |
| `GET /api/settings`, `POST /api/settings` | the settings document (see above) |
| `GET /api/status` | live state: player, sensor, Wi-Fi, error codes, radio, casts, heap |
| `POST /api/tracks?name=x.mp3`, `DELETE /api/tracks?name=x.mp3` | upload (raw body) / delete a track |
| `POST /api/player` | `{"action": "start"}` or `"stop"`: the buttons |
| `POST /api/wifi`, `POST /api/wifi/ap`, `GET /api/wifi/scan` | home network, access point password, scan (~11 s) |
| `GET /api/system` | CPU, memory, temperature, versions |
| `/dlna/...`, `/spotify_info` | DLNA renderer and Spotify zeroconf, for phones |

Any other URL redirects to `/`: that is how phones detect the captive portal.

## How to add

**A setting.** A member in the struct in `settings.h`; a line in the field table in
`settings.c` (`.optional = true`: settings saved by older firmware lack it); its label and hint
in the matching `*_FIELDS` list in `index.html` (the range comes from the firmware). The host
tests then fail on the documents they compare: add the field there too.

**A status field.** In the module's `*_status_json()`, then show it in `index.html`.

**A cast receiver.** `*_active()` and `*_stop()`, plus either PCM (`read()` of 16-bit stereo at
44.1 kHz and `volume()`) or its own `play()`; a `SOURCE_*` value and an entry in `CASTS[]` at its
priority; its start in `app_main()`; an on / off flag in `cast_settings_t` and `CAST_FIELDS`;
`*_status_json()`.

**A light color mode.** A `light_color_t` value (`settings.h`), its name in `COLOR_NAMES`
(`settings.c`), a case in the color switch of `led_task()` (`led_music.c`), a button and a hint
(`LIGHT_HINTS`) in `index.html`.

**An error code.** A `status_error_t` value (the code is the number of blinks; lower codes are
the more basic problems), its check in `check_errors()` (`status.c`), its text in `ERRORS`
(`index.html`).

**An HTTP endpoint.** A handler and a line in `uris[]` in `web.c`; `max_uri_handlers` counts
the handlers of web, DLNA and Spotify together.

## Conventions

- A module is `foo.c` / `foo.h`: `foo_start()` creates its task; getters copy a snapshot of the
  state under the module's lock. Modules learn about changes by polling; generation counters
  tell a current request from a stale one (radio, DLNA), version counters tell settings changes.
- Enum names for JSON come from a `switch` without `default`: a new value without a name stops
  the build.
- Code and comments in English, the web page in Russian. Comments say why: hardware, memory,
  protocol quirks.
- Pins only in `board.h`, shared names only in `app_config.h`.

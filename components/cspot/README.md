# cspot for the New Year star

Spotify Connect library, copied from upstream and trimmed to what the star uses:

| Directory | Source | Commit |
|---|---|---|
| `cspot/` | [philippe44/cspot](https://github.com/philippe44/cspot) (the fork squeezelite-esp32 uses; [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot) is unmaintained) | `99e5c95ad6d862a1b7845dda81fd9c8233b45b9f` |
| `bell/` | [philippe44/bell](https://github.com/philippe44/bell): utilities, HTTP client, nanopb, Tremor | `46a4a8f21f3b0bc0ccefd01915b24d74f0ab9bef` |

License: GPL-3.0 (`LICENSE.md`), nanopb zlib, Tremor BSD.

Left out of bell: codecs and audio sinks (the star has its own), the web server, MQTT, mDNS
(the star registers the service itself), nlohmann/json and fmt (`BELL_ONLY_CJSON`,
`BELL_DISABLE_FMT`), std::regex.

## Local changes

- **Mbed TLS 4 (ESP-IDF 6)**: `bell/main/utilities/Crypto.cpp` uses PSA for SHA-1 and AES-CTR,
  its own HMAC and PBKDF2 on top of them (`mbedtls_md` refuses HMAC now), the "private" bignum
  declarations (`MBEDTLS_ALLOW_PRIVATE_ACCESS`) for DH, and the hardware RNG. `TLSSocket` is rewritten on esp-tls, with server certificates checked
  against the IDF bundle.
- **Single-core ESP32-C5**: `BellTask.h` maps core 1 to "no affinity".
- **Task priorities** (`TrackPlayer.cpp`, `TrackQueue.cpp`, `MercurySession.cpp`): 3, below the
  star's audio output (5) and light music (4), so decoding ahead cannot starve them. The mercury
  task stack grew from 4 to 10 KB: reconnecting does an HTTPS request.
- **Robustness**: `AccessKeyFetcher.cpp` catches network errors (they reached the track queue
  task and aborted), checks the JSON before using it, waits 30 s after a refused token and
  reports the result through `AccessKeyFetcher::resultHandler`. `LoginBlob.cpp` checks the
  lengths in an addUser request instead of reading past the buffers, and reports
  `"deviceType": "SPEAKER"` (the cJSON variant sent the literal "deviceType").
- **Memory**: `esp_port/` puts the C++ objects (global operator new), nanopb and Tremor
  allocations in PSRAM first. The default policy keeps blocks under 4 KB in internal RAM, which
  the star needs for Wi-Fi and the I2S DMA buffers.
- **Tremor symbols**: esp_audio_codec links its own Tremor, so this copy is renamed with a
  `cspot_` prefix (`bell/external/tremor/cspot_prefix*.h`).
- **Protobuf**: the `*.pb.c` / `*.pb.h` files in `cspot/protobuf/` are generated once, so the build
  needs no Python protobuf package. To regenerate after changing a `.proto`:

  ```sh
  python3 -m venv /tmp/pb && /tmp/pb/bin/pip install protobuf grpcio-tools "setuptools<81"
  # nanopb 0.4.5 calls reflection.MakeClass, gone from protobuf 5+: use a patched copy
  git clone --depth 1 -b 0.4.5 https://github.com/nanopb/nanopb /tmp/nanopb
  sed -i 's/reflection.MakeClass(desc)()/__import__("google.protobuf.message_factory", fromlist=["x"]).GetMessageClass(desc)()/' \
      /tmp/nanopb/generator/nanopb_generator.py
  cd cspot && for p in protobuf/*.proto; do /tmp/pb/bin/python /tmp/nanopb/generator/nanopb_generator.py -I . -D . $p; done
  ```

## Requirements outside the code

- Spotify Premium on the account that picks the star in the app.
- A Client ID / Secret of an app from the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
  (entered on the star's settings page): since December 2025 cspot gets the token for the audio
  file locations with the client credentials grant of such an app. Since February 2026 the
  dashboard needs Premium and allows one development-mode app per developer.

# cspot для новогодней звезды

Библиотека Spotify Connect, скопированная из upstream и урезанная до того, что использует звезда:

| Каталог | Источник | Коммит |
|---|---|---|
| `cspot/` | [philippe44/cspot](https://github.com/philippe44/cspot) (форк, который использует squeezelite-esp32; [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot) больше не поддерживается) | `99e5c95ad6d862a1b7845dda81fd9c8233b45b9f` |
| `bell/` | [philippe44/bell](https://github.com/philippe44/bell): утилиты, HTTP-клиент, nanopb, Tremor | `46a4a8f21f3b0bc0ccefd01915b24d74f0ab9bef` |

Лицензия: GPL-3.0 (`LICENSE.md`), nanopb — zlib, Tremor — BSD.

Из bell не взяты: кодеки и аудиовыходы (у звезды свои), веб-сервер, MQTT, mDNS (звезда сама
регистрирует сервис), nlohmann/json и fmt (`BELL_ONLY_CJSON`, `BELL_DISABLE_FMT`), std::regex.

## Локальные изменения

- **Mbed TLS 4 (ESP-IDF 6)**: `bell/main/utilities/Crypto.cpp` использует PSA для SHA-1 и
  AES-CTR, собственные HMAC и PBKDF2 поверх них (`mbedtls_md` теперь отказывается делать HMAC),
  «приватные» объявления bignum (`MBEDTLS_ALLOW_PRIVATE_ACCESS`) для DH и аппаратный ГСЧ.
  `TLSSocket` переписан на esp-tls, сертификаты сервера проверяются по набору из IDF.
- **Одноядерный ESP32-C5**: `BellTask.h` превращает ядро 1 в «без привязки к ядру».
- **Приоритеты задач** (`TrackPlayer.cpp`, `TrackQueue.cpp`, `MercurySession.cpp`): 3, ниже
  вывода звука звезды (5) и цветомузыки (4), чтобы декодирование наперёд не отнимало у них время.
  Стек задачи mercury вырос с 4 до 10 КБ: при переподключении она делает HTTPS-запрос.
- **Надёжность**: `AccessKeyFetcher.cpp` ловит сетевые ошибки (они доходили до задачи очереди
  треков и роняли её), проверяет JSON перед использованием, ждёт 30 с после отказа в токене и
  сообщает результат через `AccessKeyFetcher::resultHandler`. `LoginBlob.cpp` проверяет длины в
  запросе addUser, а не читает за пределами буферов, и отправляет `"deviceType": "SPEAKER"`
  (вариант с cJSON отправлял буквально "deviceType").
- **Память**: `esp_port/` размещает объекты C++ (глобальный operator new), выделения nanopb и
  Tremor в первую очередь в PSRAM. Политика по умолчанию оставляет блоки меньше 4 КБ во
  внутренней RAM, а она нужна звезде для Wi-Fi и буферов I2S DMA.
- **Символы Tremor**: esp_audio_codec линкует свой Tremor, поэтому эта копия переименована с
  префиксом `cspot_` (`bell/external/tremor/cspot_prefix*.h`).
- **Protobuf**: файлы `*.pb.c` / `*.pb.h` в `cspot/protobuf/` сгенерированы один раз, поэтому
  сборке не нужен Python-пакет protobuf. Чтобы перегенерировать их после изменения `.proto`:

  ```sh
  python3 -m venv /tmp/pb && /tmp/pb/bin/pip install protobuf grpcio-tools "setuptools<81"
  # nanopb 0.4.5 вызывает reflection.MakeClass, которого нет в protobuf 5+: берём исправленную копию
  git clone --depth 1 -b 0.4.5 https://github.com/nanopb/nanopb /tmp/nanopb
  sed -i 's/reflection.MakeClass(desc)()/__import__("google.protobuf.message_factory", fromlist=["x"]).GetMessageClass(desc)()/' \
      /tmp/nanopb/generator/nanopb_generator.py
  cd cspot && for p in protobuf/*.proto; do /tmp/pb/bin/python /tmp/nanopb/generator/nanopb_generator.py -I . -D . $p; done
  ```

## Что нужно помимо кода

- Spotify Premium на аккаунте, с которого звезду выбирают в приложении.
- Client ID / Secret приложения из [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
  (вводятся на странице настроек звезды): с декабря 2025 года cspot получает токен для адресов
  аудиофайлов через client credentials grant такого приложения. С февраля 2026 года для
  дашборда нужен Premium, и разрешено одно приложение в режиме разработки на разработчика.

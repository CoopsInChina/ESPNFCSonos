# ESP NFC Sonos

A standalone ESP32-S3 device with a round display: shows the current
Sonos/Spotify album art fullscreen, and plays a Spotify album/track/playlist
when an NFC tag is tapped against it. Spin-off of two sibling projects:

- **ESP Sonos Controller** (`/Users/garry.cooper/Sonos`) — the original
  NFC-tag-triggers-Sonos-Spotify-playback project, built around a PN532
  reader. This project reuses its tag text format and Mifare Classic NDEF
  reading scheme.
- **Grill & Groove** (`/Users/ESP32MusicAndMeat`) — the ESP32-S3 + round
  display + album art rendering pipeline this project's display/JPEG/LVGL
  code is ported from.

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-LCD-1.85 (non-touch variant).
  ESP32-S3R8 (16MB flash, 8MB Octal PSRAM), ST77916 round QSPI LCD,
  TCA9554PWR I2C IO expander, PCM5101 I2S DAC.
- **NFC module:** a cheap, unlabeled-beyond-silkscreen "HW-033" RC522-based
  UART bridge module. Its wire protocol is **not documented anywhere** —
  see [`docs/NFC_PROTOCOL.md`](docs/NFC_PROTOCOL.md) for everything learned
  about it the hard way. If you're sourcing a module fresh, a real PN532
  breakout (I2C/SPI) with known libraries would be far less effort; this
  project ended up reverse-engineering the HW-033's protocol instead of
  swapping hardware.
- **Speaker:** any Sonos speaker on the same network, with a linked Spotify
  account.

### Wiring

| ESP32 pin (Waveshare UART header) | NFC module pin |
|---|---|
| GND | GND |
| 3V3 *or* an external supply (see note) | VCC |
| GPIO43 (TXD) | RX |
| GPIO44 (RXD) | TX |

ESP32 TX goes to the module's RX, and ESP32 RX comes from the module's TX
(crossed over, as usual for UART), plus a common GND. Module VCC per its
own supply requirements (this specific module's listing recommends 5V; see
`docs/NFC_PROTOCOL.md` for the power/logic-level reasoning). Pin
assignments live in [`src/board_config.h`](src/board_config.h).

The display, I2C expander, and I2S DAC are all onboard the Waveshare
board — no extra wiring needed for those.

## Status

| Feature | Status |
|---|---|
| Build-time WiFi config (no captive portal/web server) | Done |
| Boot status screens (WiFi/speaker/connected) | Built, **needs a visual confirmation pass on hardware** |
| Sonos discovery, configurable target speaker | Done |
| Fullscreen album art + loading spinner | Done |
| NFC tag read (UID + NDEF text) | Done — real UID detection and NDEF parsing confirmed on hardware |
| NFC → Spotify playback on tap | Done — confirmed end-to-end with a **temporary hardcoded test URI** (see below); needs a real NDEF-written tag to fully retire that override |
| Good/bad tap beep feedback | Done |
| Album art download/scale speed | Was 10-50s, now consistently ~1-2s — see `docs/NFC_PROTOCOL.md`'s note on the UART console conflict, and the direct-CDN resize fix in `album_art.c` |

### Known temporary hack

`src/nfc_reader.c`'s `nfc_reader_poll()` has a **hardcoded UID → Spotify URI
override** for the one test tag used during development (a blank Mifare
Classic card, UID `82BA2006`) — the only tag on hand while building this.
Remove it once you have a tag actually written with NDEF text in the
`service:type:id` format (see below), and confirm the real NDEF read path
works against it.

## Tag format

Tags are written with an NDEF Text record whose payload is a bare
`"service:type:id"` string — same scheme as the ESP Sonos Controller
project, e.g.:

```
spotify:album:2dfTV7CktUEBkZCHiB7VQB
spotify:track:4uLU6hMCjMI75M1A2tKUQC
spotify:playlist:37i9dQZF1DXcBWIGoYBM5M
```

Only `spotify:` is supported — `sonos_controller.c`'s
`build_spotify_uri_meta()` rejects anything else. Currently only **Mifare
Classic 1K** tags (4-byte UID) are supported for reading; NTAG21x tags
(7-byte UID) would need a different read path (the sibling PN532 project
has one; this project doesn't yet — see Outstanding work below).

Writing tags is out of scope for this project — use the ESP Sonos
Controller project or any NFC Tools app for that; this project only reads.

## Getting started

1. **Configure WiFi + target speaker.** Copy/create `src/wifi_creds.h`
   (gitignored — never commit real credentials):

   ```c
   #pragma once
   #define WIFI_SSID     "YourNetwork"
   #define WIFI_PASSWORD "YourPassword"
   #define SONOS_TARGET_SPEAKER_NAME "Move 2"   // exact Sonos room name
   ```

2. **Build & flash** (PlatformIO, ESP-IDF framework):

   ```bash
   pio run -t upload
   pio device monitor
   ```

   If you change `sdkconfig.defaults`, delete the generated
   `sdkconfig.album_nfc` first — PlatformIO/ESP-IDF won't re-apply defaults
   over an existing generated config:

   ```bash
   rm sdkconfig.album_nfc
   pio run -t upload
   ```

3. **Tap a tag.** One beep + spinner = good tap, playback starting. Two
   beeps = tag present but unreadable/not a recognised URI.

## Configuration reference

All tunables live in [`src/app_config.h`](src/app_config.h):

| Setting | Meaning |
|---|---|
| `DISPLAY_WIDTH`/`HEIGHT` | Panel resolution (360x360, round) |
| `WIFI_CONNECT_TIMEOUT_MS` | How long to wait for WiFi before showing "No WiFi" |
| `SONOS_DISCOVERY_TIMEOUT_MS` | SSDP discovery window |
| `SONOS_POLL_INTERVAL_MS` | How often to poll the speaker for current track |
| `SPOTIFY_DEFAULT_SID`/`SN` | Cold-start Spotify service params (learned from any currently-playing Spotify track if available) |
| `ART_MAX_DOWNLOAD_BYTES` | Download buffer cap (768KB — headroom over real covers) |
| `NFC_UART_BAUD` | 115200, confirmed from the module's manufacturer listing |
| `NFC_POLL_INTERVAL_MS` | How often the NFC loop checks for a tag |
| `BEEP_FREQ_HZ`/`DURATION_MS`/`GAP_MS` | Tap feedback tone |

Hardware pins: [`src/board_config.h`](src/board_config.h).

## Architecture

| File | Responsibility |
|---|---|
| `main.c` | Boot sequence, status screens, the main NFC-tap loop |
| `wifi_manager.c` | STA connect using build-time credentials |
| `tca9554.c` | I2C IO expander — drives the LCD's real reset line |
| `display.c` | ST77916 QSPI panel init via `esp_lvgl_port` |
| `ui_boot.c` | Boot glyph + status-message screens |
| `album_art.c` | HTTP download, JPEG decode (TJpgDec), nearest-neighbor scale to the panel |
| `nfc_reader.c` | HW-033 UART protocol, tag detection, NDEF text read |
| `sonos_controller.c` | SSDP discovery, SOAP control, direct (server-free) Spotify playback, track polling |
| `buzzer.c` | I2S tone generation through the onboard DAC (no discrete piezo) |
| `globals.c` | Shared state: network mutex, inter-request cooldown timing |

## Troubleshooting

- **Stale sdkconfig.** If `sdkconfig.defaults` changes don't seem to take
  effect, delete `sdkconfig.album_nfc` and rebuild — this is a recurring
  PlatformIO/ESP-IDF gotcha, not a bug in your change.
- **NFC module silent.** See `docs/NFC_PROTOCOL.md`'s "Dead ends" section
  before re-deriving any of: baud rate, wiring, or protocol framing — all
  were exhaustively tried and ruled out before finding the real protocol
  and the GPIO43/44 console conflict.
- **Upload is slow/fragile.** `platformio.ini` currently pins
  `upload_speed = 9600` — this was dropped from a higher rate after
  repeated partial-upload failures pointed at a marginal signal/power path
  during flashing, not a timeout. Worth revisiting if you get a more
  reliable USB setup.

## Outstanding work

- Confirm the boot status screens and loading spinner visually on real
  hardware (built but never explicitly screenshot-verified).
- Retire the hardcoded test-tag override in `nfc_reader.c` once a real
  NDEF-written tag is available.
- NTAG21x (7-byte UID) tag support, if needed — currently Mifare Classic
  only.
- Spotify `track`/`playlist` URI types are implemented but only `album` has
  been exercised end-to-end so far.

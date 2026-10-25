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

As of **V1.1** (firmware version string; the fixes below landed on top of
it without a version bump yet), confirmed working end-to-end on real
hardware: boot → WiFi/speaker discovery → the full
Boot/Wifi/WifiSpeaker/Ready/Art UI cycle, live playback-state reacting
(pause/resume, external track changes, NFC-tap-triggered plays), NFC tag
read, NFC-triggered Spotify playback, and idle screen dimming.

| Feature | Status |
|---|---|
| Build-time WiFi config (no captive portal/web server) | Done — captive-portal web setup is next up, see Outstanding work |
| UI states (Boot/Wifi/WifiSpeaker/Ready/Art, live-reactive) | Done — confirmed on hardware, including external track changes, pause/resume, and NFC-tap-triggered plays |
| Sonos discovery, configurable target speaker | Done |
| Fullscreen album art + loading spinner (inside the vinyl's label hole) | Done — see "Fixed bugs of note" below for several real races this went through |
| NFC tag read (UID + NDEF text) | Done — real UID detection and NDEF parsing confirmed on hardware. Self-heals from a lost boot-time antenna-enable (see `docs/NFC_PROTOCOL.md`'s Gotchas) — confirm wiring/grounding first if taps stop registering despite otherwise-healthy comms |
| NFC → Spotify playback on tap | Done — confirmed end-to-end on hardware, still via a **temporary hardcoded test URI** (see below); needs a real NDEF-written tag to fully retire that override |
| Good-tap sound / bad-tap beep feedback | Done — confirmed on hardware; tap-sound volume has gone through several rounds of reduction and may still need another pass, and a distinct custom tone (rather than just a quieter version of the current clip) is wanted |
| Live playback-state reacting (pause/resume switches Ready ↔ Art automatically) | Done — confirmed on hardware |
| Idle screen dimming (Ready screen only) | Done — see "Idle dimming" below |
| Boot-screen version label | Done |
| Album art download/scale speed | Was 10-50s, now ~2-3s typically — see `docs/NFC_PROTOCOL.md`'s note on the UART console conflict. Direct Spotify CDN links get resized to 300x300 before downloading (see `album_art.c`); art served through Sonos's `/getaa` proxy stays at 640x640 and is scaled locally — confirmed by direct testing that `/getaa` ignores every resize query param for Spotify content, so this isn't fixable from our side |
| Sonos playback reliability | Several real failure modes found and fixed (see "Sonos playback reliability" below) by probing the speaker directly during testing — some intermittent failures during this round of testing may be regional network interference rather than a firmware issue, needs retesting in a different network environment |

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

## UI states & assets

The screen is one of five states, re-evaluated continuously (not just
decided once at boot) by `main.c`'s `nfc_loop_task`:

| State | Shown when | Artwork |
|---|---|---|
| **Boot** | No WiFi | Plain vinyl record + firmware version (`V` + `FIRMWARE_VERSION` from `app_config.h`), `src/assets/boot.jpg` |
| **Wifi** | WiFi connected, no Sonos speaker found | Vinyl with a WiFi icon, `src/assets/wifi.jpg` |
| **WifiSpeaker** | WiFi + speaker found, NFC reader not yet confirmed working | Vinyl with an NFC icon (no "tap a card" text), `src/assets/wifispeaker.jpg` |
| **Ready** | WiFi + speaker + NFC all confirmed, nothing actively playing (idle/paused/stopped) | Vinyl with an NFC icon + "TAP A CARD · PLAY AN ALBUM", `src/assets/ready.jpg` |
| **Art** | WiFi + speaker found, actively playing | Fullscreen Sonos/Spotify album art |

`main.c`'s `boot_task()` walks Boot → Wifi → WifiSpeaker in order as each
condition is actually met during boot, holding each one on screen for at
least `BOOT_STEP_MIN_DISPLAY_MS` (`app_config.h`) — without that, a fast
WiFi/Sonos connection could make the live state machine's first
evaluation find every condition already true at once and jump straight
to the final state, skipping the intermediate screens entirely (this
genuinely happened before this was added).

Playback state is polled live from the speaker (AVTransport's
`GetTransportInfo` → `CurrentTransportState`), so pausing/stopping a track
switches back to **Ready** automatically, and resuming switches back to
**Art** — not just a one-time decision at boot. The NFC reader's health
(`nfc_reader_is_healthy()`) is also checked live, so Ready only ever
invites a tap when the reader is actually confirmed working.

While new art is downloading/decoding, the **Boot** vinyl is shown with a
small spinner inside its label hole (not a blank screen or stale art).

### Idle dimming

Only while continuously on **Ready** (nothing playing, nothing actionable
on screen): after `READY_DIM_IDLE_MS` with no state change, brightness
ramps linearly from 100% down to `READY_DIM_TARGET_PCT` over
`READY_DIM_RAMP_MS` (`app_config.h`). Any other state — a tap, playback
starting, losing WiFi/the speaker — snaps brightness back to 100%
immediately rather than ramping back up, so the screen is never still dim
right when something actually happened. Implemented in `main.c`'s
`nfc_loop_task`, driven by the existing PWM/LEDC backlight
(`display_set_brightness()` in `display.c`).

### Asset pipeline

Source artwork lives in `Assets/` (the original 1260×1264 PNGs — not used
directly) and `src/assets/` (the processed files actually embedded in the
firmware):

- `boot.jpg` / `wifi.jpg` / `wifispeaker.jpg` / `ready.jpg` — each source PNG cropped to a
  centered square, resized to 360×360, and saved as JPEG (quality 90).
  Decoded once at boot in `ui_assets.c` using the same TJpgDec-based
  decoder as `album_art.c`.
- `nfc_tap.wav` — converted from the provided `NfcTap.mp3` with macOS's
  built-in `afconvert`, since this project has no MP3 decoder and doesn't
  need one:

  ```bash
  afconvert -f WAVE -d LEI16@16000 -c 2 NfcTap.mp3 src/assets/nfc_tap.wav
  ```

  This matches `buzzer.c`'s I2S config exactly (16-bit, stereo, 16kHz), so
  it streams straight to the DAC with no resampling. `buzzer.c` walks the
  WAV's RIFF chunks to find the `data` chunk rather than assuming a fixed
  44-byte header (`afconvert`'s output pads it further).

All four files are embedded into the firmware as **plain generated C byte
arrays** (`src/assets/boot_jpg.c`, `wifi_jpg.c`, `wifispeaker_jpg.c`, `ready_jpg.c`,
`nfc_tap_wav.c` + matching `.h` files), run through
`src/assets/generate.py`:

```bash
python3 src/assets/generate.py
```

This is deliberately *not* `idf_component_register`'s `EMBED_FILES` —
that mechanism generates mangled/doubled intermediate `.S`-file paths
under PlatformIO's ESP-IDF integration (it assumes `idf.py`'s native
directory layout), which fails the build outright. Plain C sources have
no such issue and are picked up by `src/CMakeLists.txt`'s normal `*.c`
glob like any other source file.

**To replace any of these assets:** overwrite the matching raw file in
`src/assets/` (`boot.jpg`/`wifi.jpg`/`wifispeaker.jpg`/`ready.jpg`/`nfc_tap.wav` — keeping
the same name/format/dimensions), run `python3 src/assets/generate.py` to
regenerate the C sources, then rebuild. No other code changes needed
unless the dimensions or audio format change.

`Assets/Speaker.png` is an unused leftover from an earlier draft (a
"speaker found, no WiFi" state that turned out to be impossible, since
finding a speaker requires WiFi first) — safe to delete.

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

3. **Tap a tag.** A good tap plays the NFC-tap chime and starts playback
   (screen drops to the loading vinyl, then the album art once it's
   decoded). Two buzzer beeps = tag present but unreadable/not a
   recognised URI.

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
| `NFC_POLL_INTERVAL_MS` | How often the NFC loop checks for a tag, and re-evaluates the UI state/dimming — 100ms; idle sampling rate only, independent of the one-off per-tag SELECT/AUTH/READ cost and of `ANTENNA_REASSERT_INTERVAL_MS` |
| `BEEP_FREQ_HZ`/`DURATION_MS`/`GAP_MS` | Synthesized bad-tap beep tone (good tap uses the `nfc_tap.wav` sound clip instead) |
| `TAP_LOADING_TIMEOUT_MS` | Max time the loading screen rides out a tap-triggered play before falling back if nothing ever actually started |
| `CMD_SETTLE_GRACE_MS` | How long after a tap-triggered play finishes setup to keep ignoring a stale "not playing" transport reading — see "Sonos playback reliability" |
| `READY_DIM_IDLE_MS`/`RAMP_MS`/`TARGET_PCT` | Idle screen dimming on Ready — see "Idle dimming" above |

Hardware pins: [`src/board_config.h`](src/board_config.h).

## Architecture

| File | Responsibility |
|---|---|
| `main.c` | Boot sequence, the continuous Boot/Wifi/Ready/Art UI state machine, the main NFC-tap loop |
| `wifi_manager.c` | STA connect using build-time credentials, live connection status |
| `tca9554.c` | I2C IO expander — drives the LCD's real reset line |
| `display.c` | ST77916 QSPI panel init via `esp_lvgl_port` |
| `ui_assets.c` | Decodes the embedded Boot/Wifi/Ready vinyl JPEGs once at boot (see "UI states & assets") |
| `album_art.c` | HTTP download, JPEG decode (TJpgDec), nearest-neighbor scale to the panel |
| `nfc_reader.c` | HW-033 UART protocol, tag detection, NDEF text read |
| `sonos_controller.c` | SSDP discovery, SOAP control, direct (server-free) Spotify playback, track + live transport-state polling |
| `buzzer.c` | I2S playback through the onboard DAC — the embedded NFC-tap sound clip for good taps, a synthesized tone for bad taps |
| `globals.c` | Shared state: network mutex, inter-request cooldown timing |

## Sonos playback reliability

`play_spotify_direct()` in `sonos_controller.c` (the sequence a good NFC
tap triggers: clear queue → add the Spotify URI → point the transport at
the queue → seek to track 1 → play) went through several rounds of
hardening after probing the speaker's own UPnP API directly during
testing — not just reasoning from our own logs — surfaced real failure
modes:

- **No settle delay after `AddURIToQueue`.** For a container URI
  (album/playlist), Sonos accepting the request doesn't mean it's
  resolved the container into actual tracks yet — that means Sonos
  fetching the container's contents from Spotify's catalog over the
  internet, which doesn't happen synchronously within the call. Seeking
  and playing immediately, before that settles, reliably got the speaker
  stuck in `TRANSITIONING` forever — confirmed by querying
  `GetTransportInfo` directly against the speaker, independent of this
  firmware entirely — needing a full speaker power-cycle to clear. Fixed
  with an 800ms delay before proceeding.
- **`AddURIToQueue` timing out while Sonos is already streaming
  something else.** Queue-mutating calls can be slow to answer when the
  speaker's mid-stream on whatever's currently playing. Fixed by raising
  the SOAP HTTP timeout (3s → 6s) and retrying that one call once.
- **`AddURIToQueue` reporting success (`200`) while leaving the queue
  genuinely empty.** Confirmed directly via `GetMediaInfo`'s `NrTracks`
  field reading `0` immediately after a "successful" add — which then
  made the following `Seek`/`Play` fail outright with SOAP 500s. Fixed by
  actually checking `NrTracks` after the settle delay, with one retry of
  the add, rather than trusting the HTTP status alone.
- **Album art prefetch starving the queue-verification retry.**
  `album_art.c`'s download holds the shared network mutex (`g_network_mutex`)
  for its whole multi-second transfer. If that prefetch landed while
  `cmd_task` was mid-sequence verifying the queue actually populated, its
  SOAP retry got starved long enough to misreport a tap that had actually
  succeeded as "giving up." Fixed by exposing `sonos_is_cmd_pending()`
  (`sonos_controller.c`) and having `main.c`'s `nfc_loop_task` defer firing
  `album_art_request()` while a tap-triggered play is still being set up —
  it fires on the next iteration once no longer pending, nothing is lost.
- **Stale `transport_state` reads right after setup finishes.**
  `poll_task` only refreshes `transport_state`/`is_playing` every
  `SONOS_POLL_INTERVAL_MS` on its own schedule. The instant
  `sonos_is_cmd_pending()` clears, the last-known reading can still be the
  transient `STOPPED` a queue rebuild reports *during* setup, not yet
  whatever actually follows — which was enough to cancel the tap's loading
  screen and briefly show bare Ready with no spinner. Fixed with a
  `CMD_SETTLE_GRACE_MS` window after `cmd_pending` clears (`main.c`),
  during which a "not playing" reading isn't trusted as genuinely settled.

Some playback failures during this project's testing may be regional
network interference between the Sonos speaker and Spotify's servers
rather than anything in this firmware — worth retesting in a different
network environment before chasing further "intermittent" failures as
code bugs.

## Fixed bugs of note

A few non-obvious races worth knowing about if they ever resurface:

- **Black screen the first time Art mode was ever reached.**
  `album_art.c`'s `s_art_dsc.data` pointer was only updated inside
  `album_art_update()`, which only runs while the UI is actually showing
  Art. `art_task` only waits up to `ART_CONSUMED_TIMEOUT_MS` for that to
  happen before moving on; if a track was queued/decoded while paused and
  that timeout elapsed before Art mode was ever entered, the decoded image
  was silently discarded and the pointer stayed stuck at whatever the
  buffer held *before* that decode — the pristine all-zero buffer on a
  fresh boot. Fixed by updating `s_art_dsc.data` synchronously inside
  `art_task` the instant a decode lands, not deferred to consumption.
- **Spinner animation freezing mid-tap.** `album_art.c`'s `art_task` runs
  pinned to core 1 at priority 10 for its JPEG decode/scale step (CPU-bound,
  not I/O-wait). The LVGL port's task defaults to no core affinity, so it
  could land on core 1 too and get preempted for however long that decode
  took. Fixed by pinning the LVGL port task to core 0 in `display.c`.
- **Boot-time flash of stale/corrupted screen content.** The backlight was
  turned on right after `display_init()`, before `ui_assets_init()`
  (JPEG-decoding 4 images) and `ui_screen_create()` had actually loaded the
  Boot image — showing whatever was left in the panel's own GRAM from
  before this boot (panel memory isn't cleared by a soft reset) for that
  whole window. Fixed by moving `display_set_backlight(true)` to after the
  Boot screen is actually created, in `main.c`'s `boot_task()`.
- **NFC double-taps at the faster poll interval.** The UID de-dupe latch
  cleared on the very first missed `REQUEST` reply — rare enough to not
  matter at the old 300ms poll interval, but common enough at 100ms to
  double-fire a play for a card that never actually left the reader. Fixed
  with a 3-consecutive-miss debounce before clearing the latch (see
  `docs/NFC_PROTOCOL.md`'s Gotchas).

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
- **`Source '....S' not found` during build, mentioning a jpg/wav file.**
  That's `idf_component_register`'s `EMBED_FILES` breaking under
  PlatformIO's ESP-IDF integration — don't use it for new assets here (see
  "Asset pipeline" above). Use `src/assets/generate.py` instead.
- **Compile error about a missing `_xxx_data`/`_xxx_len` symbol.** If you
  add a new asset or rename one, update both `src/assets/generate.py`'s
  `ASSETS` list and the `#include "assets/<name>.h"` + call site in
  `ui_assets.c` or `buzzer.c`.

## Outstanding work

- Tap-sound volume may still need further reduction, and a distinct
  custom tone (not just a quieter version of the current clip) is
  wanted — see `src/assets/generate.py`'s `TAP_SOUND_TARGET_PEAK`.
- Retire the hardcoded test-tag override in `nfc_reader.c` once a real
  NDEF-written tag is available.
- NTAG21x (7-byte UID) tag support, if needed — currently Mifare Classic
  only.
- Spotify `track`/`playlist` URI types are implemented but only `album` has
  been exercised end-to-end so far.
- Delete `Assets/Speaker.png` (confirmed unused).
- Retest Sonos playback reliability outside of networks that may
  interfere with Sonos/Spotify traffic (see "Sonos playback reliability"
  above) before treating any further intermittent failures as firmware
  bugs.
- Minor: a card wobbling on removal right after a good tap can trigger a
  second, confusing "bad tap" beep from a transient `anticoll` read
  failure — not a comms bug, just a possible debounce worth adding in
  `nfc_reader.c`/`main.c` if it's annoying in practice.

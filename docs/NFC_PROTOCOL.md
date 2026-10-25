# HW-033 RC522 UART Module — Reverse-Engineered Protocol

This board (silkscreen marking **"HW-033"**) is a cheap, undocumented RC522-based
RFID module wired for a UART interface instead of SPI. No datasheet or
manufacturer documentation exists for it. This document is the result of an
extensive hardware debugging session to determine its actual wire protocol —
kept here so it never has to be re-discovered.

## Hardware identification

- Main chip: a genuine **MFRC522** (confirmed via its 27.12 MHz crystal, the
  chip's standard oscillator frequency).
- A second 20-pin SOIC MCU bridges the MFRC522 to a simple UART
  command/response protocol — this is **not** the bare chip's native UART
  register interface (NXP MFRC522 datasheet §8.1.3), which was tried and
  ruled out (see "Dead ends" below).
- 4-pin header (the one actually used): `GND / TX / RX / VCC`.
- A second, unlabeled 4-pin header (`J1`) exists with no silkscreen or
  schematic identifying its pins — not used.
- Manufacturer listing (matching this exact board photo, dimensions, and
  "TestPoint" silkscreen) confirms: **UART TTL, fixed 115200 baud**,
  ISO14443A, Mifare S50/S70 support.

## Wire format

8N1, **115200 baud** (see `NFC_UART_BAUD` in `src/app_config.h`).

Every exchange is host-initiated — the module never speaks on its own.
Frames are:

```
HEADER  CMD  [data...]  TAIL
 0x7F        0..N bytes  0xF7 (OK) or 0x55 (ERR)
```

The host always closes its own request frames with `0xF7`; the module
closes its reply frames with `0xF7` on success or `0x55` on error.
Multi-byte replies carry their payload between `HEADER` and `TAIL`, e.g. a
4-byte UID reply is `7F <uid0> <uid1> <uid2> <uid3> F7` (6 bytes total).

## Commands

| Command | Byte | Request data | Reply data |
|---|---|---|---|
| RESET | `0x00` | — | — (ack only) |
| ANT_ON | `0x01` | — | — (ack only) |
| ANT_OFF | `0x02` | — | — (ack only) |
| REQUEST | `0x03` | 1 byte: `0x52` (REQ_ALL) or `0x26` (REQ_NOT_HALT) | 2-byte tag type (ATQA) |
| ANTICOLL | `0x04` | — | 4-byte UID |
| SELECT | `0x05` | 4-byte UID | — (ack only) |
| AUTH | `0x06` | 12 bytes: authMode(1) + blockAddr(1) + key(6) + UID(4) | — (ack only) |
| READ | `0x07` | 1 byte: block address | 16 bytes: block data |
| WRITE | `0x08` | 17 bytes: blockAddr(1) + data(16) | — (ack only) |
| VALUE | `0x09` | mode(1) + blockAddr(1) + value(4, LSB first) | — (ack only) |
| BAK_VALUE | `0x0A` | srcBlock(1) + targetBlock(1) | — (ack only) |
| HALT | `0x0B` | — | — (ack only) |

Auth mode bytes: Key A = `0x60`, Key B = `0x61` (standard Mifare values).

This exact command set and framing were confirmed against a working Arduino
library for this specific module:
[TCL47-GITHUB/RC522_UART](https://github.com/TCL47-GITHUB/RC522_UART).

## Init handshake

1. Send a **bare, unframed** `0x01` byte ("enter normal mode"). If the
   module is already in normal mode it may not reply; if it does, it
   replies with a bare `0xAA`. This is *not* the same as the framed
   `ANT_ON` command, which also happens to use byte value `0x01` but inside
   the `0x7F ... 0xF7` envelope.
2. If no `0xAA` came back, send a framed `RESET` (`0x00`) and wait for an
   ack.
3. Send a framed `ANT_ON` (`0x01`) and wait for an ack.

See `nfc_reader_init()` in `src/nfc_reader.c`.

## Tap-read sequence

```
REQUEST(REQ_ALL)  -> tag type (2 bytes)   [TAIL_ERR / timeout = no card]
ANTICOLL()        -> UID (4 bytes)
SELECT(uid)       -> ack
AUTH(KeyA, block 4, key, uid) -> ack       (try candidate keys in order)
READ(block) x N   -> 16 bytes each
HALT()            -> ack
```

- **HALT must come last**, after all reads — halting first was tried and
  broke every read after the very first tap (the card goes dormant and
  won't answer `SELECT`/`AUTH` once halted).
- Repeated `REQUEST` with the card still present reliably returns the same
  UID; removing the card causes `REQUEST` to time out, which is also the
  signal our code uses to reset its own tap de-dupe state (see "Gotchas"
  below).

## NDEF text extraction (Mifare Classic 1K)

Identical sector/key/TLV scheme to the sibling **ESP Sonos Controller**
project's PN532-based reader
(`/Users/garry.cooper/Sonos/src/pn532_nfc_manager.cpp`,
`readNDEFTextMifareClassic()`), since tags are written by that project:

- Try Key A in order: NFC Forum NDEF key `D3 F7 D3 F7 D3 F7`, factory
  default `FF FF FF FF FF FF`, MAD key `A0 A1 A2 A3 A4 A5`.
- Sectors 1–3 (blocks 4–15), 3 data blocks of 16 bytes per sector (the 4th
  block of each sector is the trailer — skipped).
- Parse the resulting byte stream as NFC Forum TLV, looking for the NDEF
  Message TLV (type `0x03`), then a single Well-Known Text record
  (TNF `0x01`, type `'T'`), extracting the text after its language-code
  prefix.
- The payload is expected to be a bare `service:type:id` string, e.g.
  `spotify:album:2dfTV7CktUEBkZCHiB7VQB`.

See `read_ndef_text()` in `src/nfc_reader.c`.

## Dead ends (don't repeat these)

- **Baud rate guessing.** Swept 9600/2400/19200/38400/57600/115200 before
  finding the real blocker (below). 115200 is correct per the manufacturer
  listing.
- **PN532-style framed commands** (`00 00 FF ... FE D4 02 2A 00`
  GetFirmwareVersion). Got no response — wrong protocol family entirely.
- **Bare MFRC522 native UART register protocol** (NXP datasheet §8.1.3:
  single address byte in, single register-value byte out, 9600 baud
  default, requires a `0x55` wake byte + polling address 0 until ready).
  This board does *not* expose that interface directly — it has its own
  bridging MCU with the command set documented above instead.
- **GPIO43/44 conflict.** These are the ESP32-S3's own hardwired
  `U0TXD`/`U0RXD` (UART0 console) pins. With the default
  `CONFIG_ESP_CONSOLE_UART_DEFAULT`, the chip continuously drives its own
  log output onto the same physical pins wired to this module — a standing
  conflict that caused total silence regardless of protocol or baud tried.
  Fixed by setting `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in
  `sdkconfig.defaults` (the `platformio device monitor` session keeps
  working unaffected — it was already reading the *secondary* USB-JTAG
  console channel, not UART0).

## Gotchas

- **De-dupe latch.** The code tracks the last-seen UID to avoid re-firing
  while a tag rests on the reader, but this latch must be cleared whenever
  `REQUEST` times out (no card present) — otherwise re-presenting the
  *same* physical tag a second time is silently swallowed forever. See the
  comment at the top of `nfc_reader_poll()`. **Don't clear it on the first
  miss**, though: a single missed `REQUEST` reply (noise, a brief field
  dropout, slight card wobble) while the card is still genuinely resting on
  the reader is rare at a slow poll interval but common enough at a fast
  one (100ms) to double-fire a play for a card that never left. Requires
  `UID_ABSENCE_CONFIRM_MISSES` (3) consecutive misses before clearing.
- **HALT ordering**, as above — halt only after you're done with the card
  for this tap, never before reading it.
- **`ANT_ON` (and the reset/enter-normal-mode handshake) run exactly once,
  at boot, in `nfc_reader_init()`.** A transient wiring glitch during
  that specific boot-time window can leave the module fully reachable —
  `REQUEST` still gets a clean, deterministic reply every poll — with its
  antenna never actually turned on, so it senses nothing until a full
  power-cycle re-runs init. This looks identical to "the module just
  doesn't see tags" from the log alone; the tell is that comms otherwise
  look completely healthy. Confirmed on real hardware (a flaky ground
  connection specifically), not just theorized. Fixed by periodically
  re-asserting `ANT_ON` in `nfc_reader_poll()` (`reassert_antenna_if_due()`,
  every `ANTENNA_REASSERT_INTERVAL_MS`) — idempotent and cheap, so a lost
  boot-time init self-heals without a manual reboot.
- **A well-formed `TAIL_ERR` reply isn't a link health problem.** The
  periodic `ANT_ON` reassert above used to treat *any* non-`TAIL_OK` reply
  as "unhealthy," including a clean `TAIL_ERR` NAK (plausibly "already on")
  — which silently reverted the UI from Ready to WifiSpeaker every reassert
  interval even though the module was responding fine. Link health
  (`nfc_reader_is_healthy()`) now tracks whether the exchange was
  well-formed at all (logged as `NFC_LINK_NORMAL`, OK or ERR both count),
  not the specific OK-vs-ERR outcome of one command.
- **Command echo isn't limited to `REQUEST`.** Some firmware echoes the
  exact frame just sent back before its real reply — originally only
  handled for `REQUEST`'s known echo pattern, which missed it when the
  periodic `ANT_ON` reassert got echoed too, misclassifying it as
  corrupt/abnormal. `recv_packet()` now checks incoming bytes against
  whatever `send_packet()` actually last transmitted, for any command, as
  a prefix match checked before (not only once) the reply's own expected
  length is reached — a short reply like `ANT_ON`'s 2-byte ack would
  otherwise get judged against a still-incomplete longer echo.
- **Ground matters, and more isn't always better.** A real failure mode
  seen on this hardware: with the module's UART ground *not* connected to
  the ESP32, taps produced real data mixed with garbage; connecting that
  ground directly to the external 5V supply's ground *broke* comms
  entirely (TX echoed back on RX, followed by noise) rather than fixing
  it — the opposite of normal behavior. That pattern means the two
  supplies don't share a clean common ground reference, not that the
  wiring just needs a ground wire added. Measuring the DC voltage between
  the two "grounds" (without bridging them) before doing any more direct
  connecting is the safe way to quantify this.

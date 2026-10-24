#pragma once

// Beep feedback via the onboard PCM5101 I2S DAC + amp + speaker (this board
// has no discrete piezo buzzer GPIO — unlike the sibling Grill & Groove
// board, audio here only exists as an I2S tone through the real speaker
// path). Pins: board_config.h I2S_BCLK_GPIO/I2S_WS_GPIO/I2S_DOUT_GPIO
// (currently UNVERIFIED placeholders — see board_config.h).

void buzzer_init(void);

// Plays `count` short square-wave beeps (BEEP_FREQ_HZ/BEEP_DURATION_MS from
// app_config.h), with BEEP_GAP_MS between them. Runs on its own task —
// returns immediately, does not block the caller.
//   count=2 -> bad tap
void buzzer_beep(int count);

// Plays the embedded NFC-tap sound (Assets/ui/nfc_tap.wav) in place of a
// synthesized beep for a good tap. Same queued/non-blocking behavior as
// buzzer_beep().
void buzzer_play_tap_sound(void);

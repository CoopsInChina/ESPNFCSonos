#pragma once

// Boot screens. Call display_lock()/unlock() around these.

// Centered WiFi glyph, shown immediately while we connect.
void ui_boot_show(void);

// Centered status text (e.g. "No WiFi.\nCheck Settings", "No Speaker Found",
// "Speaker Connected!\n<name>"). Fades in from whatever screen was active.
void ui_boot_show_message(const char *text);

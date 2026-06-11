// ─────────────────────────────────────────────────────────────
// main.cpp — ESP32 Base Controller
//
// Hardware summary:
//   LCD (parallel 4-bit)
//   Encoder
//   I2C master
//   ALERT in
//
// First-time setup:
//   Call iic_writeUDID(uniqueSerial) ONCE per board.
//   Uncomment in setup(), flash, recomment.
//
// Boot sequence:
//   1. led_init()     — set up own RGB LED, default red
//   2. iic_init()     — set up Wire master, ALERT interrupt,
//                       run initial GET_UDID enumeration for RP2040s
//   3. lcd_encoder_init() — set up LCD and encoder
//   Loop:
//   4. iic_update()   — handle ALERT, periodic drawer scan
//   5. encoder_update()— handle encoder input, LCD redraw
// ─────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <iic_manager.h>
#include <lcd_encoder_manager.h>
#include <led_manager.h>

// ─────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  lcd_encoder_init();
  // lcd_encoder_init() clears LCD and sets up encoder pins.
  // First redraw happens on first encoder_update() call.

  iic_init();
  // iic_init() runs initial enumeration automatically.
  // All RP2040 drawers powered on before ESP32 will be found.
  // Drawers powered after boot are detected via ALERT interrupt.

  // ── FIRST TIME ONLY: write own UDID ───────────────────────
  // iic_writeUDID(0x00000100); // ESP32 unit 1
}

// ─────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  iic_update();
  // Handles:
  //   - ALERT interrupt response (re-enumeration of new drawers)
  //   - Periodic drawer scanner every 5 seconds
  //   - Removes departed drawers and their ATtiny85 lists

  encoder_update();
  // Handles:
  //   - Encoder rotation (scroll menu)
  //   - Short press (select / execute command)
  //   - Long press (go back one menu level)
  //   - LCD redraw when state changes
}

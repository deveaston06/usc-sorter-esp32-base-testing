// ─────────────────────────────────────────────────────────────
// led_manager.cpp — ESP32 Base Controller
//
// Responsibilities:
//   - Track LED state per I2C module (RP2040 and ATtiny85)
//   - Provide status icon char for LCD display
//     'G' = green active
//     'R' = red / default
//     '?' = unknown / never commanded
// ─────────────────────────────────────────────────────────────

#include <led_manager.h>

static uint8_t moduleState[ADDR_SPACE];

// ─────────────────────────────────────────────────────────────
// PUBLIC: PER-MODULE STATE TRACKING
// ─────────────────────────────────────────────────────────────

// Call after sending LED command to a module
void led_setModuleState(uint8_t addr, bool green) {
  if (addr < ADDR_SPACE) {
    moduleState[addr] = green ? STATE_GREEN : STATE_RED;
  }
}

// Reset module state when it is removed or re-enumerated
void led_clearModuleState(uint8_t addr) {
  if (addr < ADDR_SPACE) {
    moduleState[addr] = STATE_UNKNOWN;
  }
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: STATUS ICON FOR LCD
// Returns single char: 'G', 'R', or '?'
// ─────────────────────────────────────────────────────────────
char led_getModuleIcon(uint8_t addr) {
  if (addr >= ADDR_SPACE)
    return '?';
  switch (moduleState[addr]) {
  case STATE_GREEN:
    return 'G';
  case STATE_RED:
    return 'R';
  default:
    return '?';
  }
}

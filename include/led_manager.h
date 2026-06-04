#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>

// ── Per-module state table ────────────────────────────────────
// I2C addresses 0x00-0x7F (128 entries)
// 0 = unknown, 1 = red, 2 = green
#define STATE_UNKNOWN 0
#define STATE_RED 1
#define STATE_GREEN 2
#define ADDR_SPACE 128

void led_setModuleState(uint8_t addr, bool green);
void led_clearModuleState(uint8_t addr);
char led_getModuleIcon(uint8_t addr);
void encoder_requestRedraw();

#endif // !LED_MANAGER_H

#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>
#include <iic_manager.h>

// ── State constants ───────────────────────────────────────────
#define STATE_UNKNOWN 0
#define STATE_RED 1
#define STATE_GREEN 2

// ── Address space ─────────────────────────────────────────────
#define ADDR_DRAWER_BASE 0x10
#define ADDR_DRAWER_MAX 0x1F // 16 possible drawers
#define ADDR_TINY_BASE 0x20
#define ADDR_TINY_MAX 0x77 // 88 possible ATtiny85s
#define ADDR_SPACE 0x80    // Full 7-bit I2C space

typedef struct {
  uint8_t addr;          // Drawer I2C address
  uint8_t state;         // Drawer LED state
  uint8_t tinyCount;     // How many ATtiny85s tracked under this drawer
  uint8_t tinyStates[8]; // Per-ATtiny85 state (by index, not address)
  uint8_t tinyAddrs[8];  // ATtiny85 addresses
} DrawerLEDState;

// ── Public API ────────────────────────────────────────────────

// Drawer management
void led_registerDrawer(uint8_t addr);
void led_removeDrawer(uint8_t addr);

// ATtiny85 management (linked to a drawer)
void led_registerTiny(uint8_t drawerAddr, uint8_t tinyAddr);
void led_removeTiny(uint8_t drawerAddr, uint8_t tinyAddr);
void led_clearAllTinies(uint8_t drawerAddr);

// Set LED state
void led_setDrawerState(uint8_t addr, bool green);
void led_setTinyState(uint8_t drawerAddr, uint8_t tinyAddr, bool green);

// Query state
uint8_t led_getState(uint8_t addr);
char led_getModuleIcon(uint8_t addr);
bool led_isGreen(uint8_t addr);

// Bulk operations
void led_clearAll(void);
void led_setAllRed(uint8_t drawerAddr);

#endif // !LED_MANAGER_H

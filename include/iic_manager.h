#ifndef IIC_MANAGER_H
#define IIC_MANAGER_H

#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <led_manager.h>

// ── I2C pins ──────────────────────────────────────────────────
#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_FREQ 100000

// ── ALERT pin ─────────────────────────────────────────────────
#define PIN_ALERT 23
#define ALERT_DEBOUNCE_MS 20

// ── Commands ──────────────────────────────────────────────────
#define CMD_GET_UDID 0x01
#define CMD_ASSIGN_ADDR 0x02
#define CMD_PREPARE_ARP 0x03
#define CMD_SCAN_MODULES 0x05
#define CMD_SYNC_PSA 0x06
#define CMD_LED_GREEN 0x10
#define CMD_LED_RED 0x11
#define CMD_LED_BLUE 0x12

// ── Addresses ─────────────────────────────────────────────────
#define ADDR_ARP_DEFAULT 0x55

// ── EEPROM layout ─────────────────────────────────────────────
// 0-8:    Own UDID (9 bytes)
// 9:      PSA count (1 byte)
// 10-129: PSA entries, 12 x (UDID 9 + addr 1) = 120 bytes
#define EE_UDID_START 0
#define EE_PSA_COUNT 9
#define EE_PSA_LIST 10
#define PSA_ENTRY_SIZE 10 // UDID(9) + addr(1)
#define PSA_MAX_ENTRIES 12
#define EEPROM_SIZE 256 // increased from 64

// ── UDID constants ────────────────────────────────────────────
#define UDID_SIZE 9
#define DEVICE_TYPE_ESP32 0x3232
#define PROTOCOL_VERSION 0x0001
#define CAPABILITIES 0x04 // master only

// ── Drawer table ──────────────────────────────────────────────
#define MAX_DRAWERS 8
#define MAX_TINY_PER_DRAWER 8
#define DRAWER_START_ADDRESS 0x10

struct DrawerEntry {
  uint8_t addr;
  uint8_t tinyAddrs[MAX_TINY_PER_DRAWER];
  uint8_t tinyUdids[MAX_TINY_PER_DRAWER]
                   [UDID_SIZE]; // per-slot UDID from SCAN_MODULES
  uint8_t tinyCount;
};

struct PSAEntry {
  uint8_t udid[UDID_SIZE];
  uint8_t addr;
};

#define SCAN_INTERVAL_MS 5000

void iic_init();
void iic_update();
void iic_writeUDID(uint32_t serialNumber);
uint8_t iic_getDrawerCount();
uint8_t iic_getDrawerAddr(uint8_t idx);
uint8_t iic_getTinyCount(uint8_t drawerIdx);
uint8_t iic_getTinyAddr(uint8_t drawerIdx, uint8_t tinyIdx);
void iic_sendLedGreen(uint8_t drawerAddr, uint8_t tinyAddr);
void iic_sendLedRed(uint8_t drawerAddr, uint8_t tinyAddr);
void iic_addPSA(uint8_t drawerAddr, uint8_t tinyAddr);
void iic_removePSA(uint8_t tinyAddr);
bool iic_isPSA(uint8_t tinyAddr);
uint8_t iic_getPSACount();
uint8_t iic_getPSAAddr(uint8_t idx);
bool iic_isPSAAvailable(uint8_t idx);
uint8_t iic_getPSADrawerAddr(uint8_t psaIdx);

#endif // !IIC_MANAGER_H

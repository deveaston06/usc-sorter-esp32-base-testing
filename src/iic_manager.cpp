#include <iic_manager.h>

static uint8_t drawerCount = 0;
static uint8_t nextDrawerAddr = DRAWER_START_ADDRESS;

// ── PSA list (source of truth on ESP32) ──────────────────────
static PSAEntry psaEntries[PSA_MAX_ENTRIES];
static uint8_t psaCount = 0;
static DrawerEntry drawers[MAX_DRAWERS];

// ── Own UDID ──────────────────────────────────────────────────
static uint8_t udid[UDID_SIZE];

// ── Flags ─────────────────────────────────────────────────────
static volatile bool alertPending = false;
static volatile uint32_t alertDebounceMs = 0;

static uint32_t lastScanMs = 0;

// ─────────────────────────────────────────────────────────────
// ALERT ISR
// ─────────────────────────────────────────────────────────────
static void IRAM_ATTR onAlert() {
  alertPending = true;
  alertDebounceMs = millis();
}

// ─────────────────────────────────────────────────────────────
// UDID HELPERS
// ─────────────────────────────────────────────────────────────
static void udid_load() {
  for (uint8_t i = 0; i < UDID_SIZE; i++) {
    udid[i] = EEPROM.read(EE_UDID_START + i);
  }
}

void iic_writeUDID(uint32_t serialNumber) {
  uint16_t pv = PROTOCOL_VERSION;
  uint16_t dt = DEVICE_TYPE_ESP32;
  EEPROM.write(0, (uint8_t)(pv >> 8));
  EEPROM.write(1, (uint8_t)(pv));
  EEPROM.write(2, (uint8_t)(dt >> 8));
  EEPROM.write(3, (uint8_t)(dt));
  EEPROM.write(4, (uint8_t)(serialNumber >> 24));
  EEPROM.write(5, (uint8_t)(serialNumber >> 16));
  EEPROM.write(6, (uint8_t)(serialNumber >> 8));
  EEPROM.write(7, (uint8_t)(serialNumber));
  EEPROM.write(8, CAPABILITIES);
  EEPROM.commit();
}

// ─────────────────────────────────────────────────────────────
// UPSTREAM WIRE RETRY WRAPPERS
// Retries if ALERT is bouncing during transmission
// ─────────────────────────────────────────────────────────────
static uint8_t wire_write(uint8_t addr, uint8_t cmd) {
  for (uint8_t i = 0; i < WIRE_RETRY_COUNT; i++) {
    if (alertPending && (millis() - alertDebounceMs) < ALERT_DEBOUNCE_MS) {
      delay(ALERT_DEBOUNCE_MS);
      continue;
    }
    Wire.beginTransmission(addr);
    Wire.write(cmd);
    if (Wire.endTransmission() == 0)
      return 0;
    delayMicroseconds(500);
  }
  return 1;
}

static uint8_t wire_write_buf(uint8_t addr, const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < WIRE_RETRY_COUNT; i++) {
    if (alertPending && (millis() - alertDebounceMs) < ALERT_DEBOUNCE_MS) {
      delay(ALERT_DEBOUNCE_MS);
      continue;
    }
    Wire.beginTransmission(addr);
    Wire.write(data, len);
    if (Wire.endTransmission() == 0)
      return 0;
    delayMicroseconds(500);
  }
  return 1;
}

static uint8_t wire_request(uint8_t addr, uint8_t len) {
  for (uint8_t i = 0; i < WIRE_RETRY_COUNT; i++) {
    if (alertPending && (millis() - alertDebounceMs) < ALERT_DEBOUNCE_MS) {
      delay(ALERT_DEBOUNCE_MS);
      continue;
    }
    uint8_t received = Wire.requestFrom(addr, len);
    if (received > 0)
      return received;
    delayMicroseconds(500);
  }
  return 0;
}

// ─────────────────────────────────────────────────────────────
// PSA LIST EEPROM PERSISTENCE
// ─────────────────────────────────────────────────────────────
static void psa_save() {
  EEPROM.write(EE_PSA_COUNT, psaCount);
  for (uint8_t i = 0; i < psaCount; i++) {
    uint16_t base = EE_PSA_LIST + (i * PSA_ENTRY_SIZE);
    for (uint8_t j = 0; j < UDID_SIZE; j++) {
      EEPROM.write(base + j, psaEntries[i].udid[j]);
    }
    EEPROM.write(base + UDID_SIZE, psaEntries[i].addr);
  }
  EEPROM.commit();
}

static void psa_load() {
  uint8_t count = EEPROM.read(EE_PSA_COUNT);
  psaCount = (count > PSA_MAX_ENTRIES) ? 0 : count;
  for (uint8_t i = 0; i < psaCount; i++) {
    uint16_t base = EE_PSA_LIST + (i * PSA_ENTRY_SIZE);
    for (uint8_t j = 0; j < UDID_SIZE; j++) {
      psaEntries[i].udid[j] = EEPROM.read(base + j);
    }
    psaEntries[i].addr = EEPROM.read(base + UDID_SIZE);
  }
}

static bool psa_contains(uint8_t addr) {
  for (uint8_t i = 0; i < psaCount; i++) {
    if (psaEntries[i].addr == addr)
      return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
// SYNC PSA
// Broadcasts full PSA entries list to every known drawer and to 0x55
// so newly powered but not yet enumerated RP2040s also receive it
// ─────────────────────────────────────────────────────────────
static void syncPSA() {
  // payload: [CMD][count][udid1 x9][addr1][udid2 x9][addr2]...
  uint8_t payload[2 + PSA_MAX_ENTRIES * (UDID_SIZE + 1)];
  payload[0] = CMD_SYNC_PSA;
  payload[1] = psaCount;
  uint8_t offset = 2;
  for (uint8_t i = 0; i < psaCount; i++) {
    for (uint8_t j = 0; j < UDID_SIZE; j++) {
      payload[offset++] = psaEntries[i].udid[j];
    }
    payload[offset++] = psaEntries[i].addr;
  }
  uint8_t len = offset;

  // send to all known drawers individually
  for (uint8_t i = 0; i < drawerCount; i++) {
    Wire.beginTransmission(drawers[i].addr);
    Wire.write(payload, len);
    Wire.endTransmission();
    delayMicroseconds(5);
  }

  // also send to ARP default to cover unresolved drawers
  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  Wire.write(payload, len);
  Wire.endTransmission();
}

// ─────────────────────────────────────────────────────────────
// DRAWER TABLE HELPERS
// ─────────────────────────────────────────────────────────────
static bool drawer_isKnown(uint8_t addr) {
  for (uint8_t i = 0; i < drawerCount; i++) {
    if (drawers[i].addr == addr)
      return true;
  }
  return false;
}

static DrawerEntry *drawer_get(uint8_t addr) {
  for (uint8_t i = 0; i < drawerCount; i++) {
    if (drawers[i].addr == addr)
      return &drawers[i];
  }
  return nullptr;
}

static void drawer_add(uint8_t addr) {
  if (drawerCount < MAX_DRAWERS && !drawer_isKnown(addr)) {
    drawers[drawerCount].addr = addr;
    drawers[drawerCount].tinyCount = 0;
    drawerCount++;
  }
}

static void drawer_remove(uint8_t addr) {
  for (uint8_t i = 0; i < drawerCount; i++) {
    if (drawers[i].addr == addr) {
      for (uint8_t j = 0; j < drawers[i].tinyCount; j++) {
        led_clearModuleState(drawers[i].tinyAddrs[j]);
      }
      led_clearModuleState(addr);
      for (uint8_t j = i; j < drawerCount - 1; j++) {
        drawers[j] = drawers[j + 1];
      }
      drawerCount--;
      return;
    }
  }
}

static uint8_t getNextDrawerAddr() {
  while (drawer_isKnown(nextDrawerAddr) && nextDrawerAddr < 0x20) {
    nextDrawerAddr++;
  }
  return nextDrawerAddr++;
}

// ─────────────────────────────────────────────────────────────
// FETCH ATTINY85 LIST FROM ONE DRAWER
// ─────────────────────────────────────────────────────────────
static void fetchTinyList(uint8_t drawerAddr) {
  DrawerEntry *d = drawer_get(drawerAddr);
  if (!d)
    return;

  if (wire_write(drawerAddr, CMD_SCAN_MODULES) != 0)
    return;

  // give RP2040 enough time for onReceive ISR to fire and prepare reply buffer
  // 5ms is conservative — ISR prepares buffer immediately on receive
  delay(5);

  uint8_t expectLen = 1 + MAX_TINY_PER_DRAWER * (1 + UDID_SIZE);
  uint8_t received = wire_request(drawerAddr, expectLen);
  if (received < 1)
    return;

  uint8_t count = Wire.read();
  if (count > MAX_TINY_PER_DRAWER)
    count = MAX_TINY_PER_DRAWER;

  d->tinyCount = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (!Wire.available())
      break;
    d->tinyAddrs[d->tinyCount] = Wire.read();
    for (uint8_t j = 0; j < UDID_SIZE; j++) {
      d->tinyUdids[d->tinyCount][j] = Wire.available() ? Wire.read() : 0xFF;
    }
    d->tinyCount++;
  }
  // drain any remaining bytes (guards against padding)
  while (Wire.available())
    Wire.read();
}

// ─────────────────────────────────────────────────────────────
// SINGLE GET_UDID CYCLE
// ─────────────────────────────────────────────────────────────
static bool getUdidCycle() {
  // Step 1: Send get UDID command to all unassigned modules
  if (wire_write(ADDR_ARP_DEFAULT, CMD_GET_UDID) != 0)
    return false;

  delayMicroseconds(200);

  // Step 2: Request UDID from the ARP address
  // During SMBus ARP, the winning slave responds
  uint8_t received = wire_request(ADDR_ARP_DEFAULT, UDID_SIZE);
  if (received < UDID_SIZE)
    return false;

  uint8_t winnerUdid[UDID_SIZE];
  for (uint8_t i = 0; i < UDID_SIZE; i++) {
    winnerUdid[i] = Wire.read();
  }

  uint8_t newAddr = getNextDrawerAddr();
  if (newAddr == 0xFF)
    return false;

  // Step 3: Assign address to winning device
  uint8_t payload[UDID_SIZE + 2];
  payload[0] = CMD_ASSIGN_ADDR;
  memcpy(&payload[1], winnerUdid, UDID_SIZE);
  payload[UDID_SIZE + 1] = newAddr;

  if (wire_write_buf(ADDR_ARP_DEFAULT, payload, sizeof(payload)) != 0)
    return false;

  delayMicroseconds(200);

  // Step 4: Verify new address works
  Wire.beginTransmission(newAddr);
  if (Wire.endTransmission() == 0) {
    drawer_add(newAddr);
    fetchTinyList(newAddr);
    return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
// FULL ENUMERATION
// Calls syncPSA() after all drawers resolved so RP2040 caches
// are always up to date with ESP32 source of truth
// ─────────────────────────────────────────────────────────────
static void runEnumeration() {
  // Detach interrupt before starting
  detachInterrupt(digitalPinToInterrupt(PIN_ALERT));

  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  Wire.write(CMD_PREPARE_ARP);
  Wire.endTransmission();
  delayMicroseconds(200);

  uint8_t maxCycles = MAX_DRAWERS;
  while (maxCycles-- > 0) {
    if (!getUdidCycle())
      break;
  }

  // push PSA truth to all drawers now that addresses are settled
  syncPSA();

  encoder_requestRedraw();
  Serial.println("Enumeration complete");

  // Wait for ALERT line to go HIGH before re-attaching interrupt
  // This prevents immediate re-triggering
  unsigned long startWait = millis();
  while (digitalRead(PIN_ALERT) == LOW) {
    if (millis() - startWait > 5000) {
      Serial.println("Warning: ALERT stuck LOW");
      break;
    }
    delayMicroseconds(10);
  }

  // Small extra delay for debounce
  delayMicroseconds(200);

  // Only re-attach if ALERT is HIGH
  if (digitalRead(PIN_ALERT) == HIGH) {
    attachInterrupt(digitalPinToInterrupt(PIN_ALERT), onAlert, FALLING);
  } else {
    // ALERT still low - set flag to retry later
    alertPending = true;
  }
}

// ─────────────────────────────────────────────────────────────
// PERIODIC SCANNER
// ─────────────────────────────────────────────────────────────
static void periodicScan() {
  bool changed = false;
  for (uint8_t i = drawerCount; i-- > 0;) {
    Wire.beginTransmission(drawers[i].addr);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
      drawer_remove(drawers[i].addr);
      changed = true;
    } else {
      fetchTinyList(drawers[i].addr);
    }
  }
  if (changed)
    encoder_requestRedraw();
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: LED COMMANDS
// ─────────────────────────────────────────────────────────────
void iic_sendLedGreen(uint8_t drawerAddr, uint8_t tinyAddr) {
  Wire.beginTransmission(drawerAddr);
  Wire.write(CMD_LED_GREEN);
  Wire.write(tinyAddr);
  Wire.endTransmission();
  led_setModuleState(tinyAddr, true);
  led_setModuleState(drawerAddr, true);
}

void iic_sendLedRed(uint8_t drawerAddr, uint8_t tinyAddr) {
  Wire.beginTransmission(drawerAddr);
  Wire.write(CMD_LED_RED);
  Wire.write(tinyAddr);
  Wire.endTransmission();
  led_setModuleState(tinyAddr, false);
  led_setModuleState(drawerAddr, false);
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: PSA MANAGEMENT
// Both call syncPSA() immediately so RP2040 caches update
// ─────────────────────────────────────────────────────────────
void iic_addPSA(uint8_t drawerAddr, uint8_t tinyAddr) {
  if (psa_contains(tinyAddr) || psaCount >= PSA_MAX_ENTRIES)
    return;

  // find UDID from drawer table
  DrawerEntry *d = drawer_get(drawerAddr);
  if (!d)
    return;
  uint8_t tinyIdx = 0xFF;
  for (uint8_t i = 0; i < d->tinyCount; i++) {
    if (d->tinyAddrs[i] == tinyAddr) {
      tinyIdx = i;
      break;
    }
  }
  if (tinyIdx == 0xFF)
    return;

  memcpy(psaEntries[psaCount].udid, d->tinyUdids[tinyIdx], UDID_SIZE);
  psaEntries[psaCount].addr = tinyAddr;
  psaCount++;
  psa_save();
  syncPSA();
}

void iic_removePSA(uint8_t tinyAddr) {
  for (uint8_t i = 0; i < psaCount; i++) {
    if (psaEntries[i].addr == tinyAddr) {
      for (uint8_t j = i; j < psaCount - 1; j++) {
        psaEntries[j] = psaEntries[j + 1];
      }
      psaCount--;
      psa_save();
      syncPSA();
      return;
    }
  }
}

bool iic_isPSA(uint8_t tinyAddr) { return psa_contains(tinyAddr); }

// ─────────────────────────────────────────────────────────────
// PUBLIC: INIT
// ─────────────────────────────────────────────────────────────
void iic_init() {
  EEPROM.begin(EEPROM_SIZE);
  udid_load();
  psa_load();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ);

  pinMode(PIN_ALERT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ALERT), onAlert, FALLING);
  Serial.println("testing");
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: UPDATE
// ─────────────────────────────────────────────────────────────
void iic_update() {
  if (alertPending) {
    if (millis() - alertDebounceMs >= ALERT_DEBOUNCE_MS) {
      alertPending = false;
      if (digitalRead(PIN_ALERT) == LOW) {
        // pin still LOW after full debounce window — real event
        detachInterrupt(digitalPinToInterrupt(PIN_ALERT));
        Serial.println("Running Enumeration");
        runEnumeration();
      }
      // else: pin returned HIGH within debounce window, discord bounce
    }
  }

  uint32_t now = millis();
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    periodicScan();
  }
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: GETTERS
// ─────────────────────────────────────────────────────────────
uint8_t iic_getDrawerCount() { return drawerCount; }
uint8_t iic_getDrawerAddr(uint8_t idx) {
  return (idx < drawerCount) ? drawers[idx].addr : 0xFF;
}
uint8_t iic_getTinyCount(uint8_t drawerIdx) {
  return (drawerIdx < drawerCount) ? drawers[drawerIdx].tinyCount : 0;
}
uint8_t iic_getTinyAddr(uint8_t drawerIdx, uint8_t tinyIdx) {
  if (drawerIdx >= drawerCount)
    return 0xFF;
  if (tinyIdx >= drawers[drawerIdx].tinyCount)
    return 0xFF;
  return drawers[drawerIdx].tinyAddrs[tinyIdx];
}

uint8_t iic_getPSACount() { return psaCount; }

uint8_t iic_getPSAAddr(uint8_t idx) {
  return (idx < psaCount) ? psaEntries[idx].addr : 0xFF;
}

// returns true if PSA address is currently enumerated in any drawer
bool iic_isPSAAvailable(uint8_t idx) {
  if (idx >= psaCount)
    return false;
  uint8_t addr = psaEntries[idx].addr;
  for (uint8_t i = 0; i < drawerCount; i++) {
    for (uint8_t j = 0; j < drawers[i].tinyCount; j++) {
      if (drawers[i].tinyAddrs[j] == addr)
        return true;
    }
  }
  return false;
}

// returns drawer address that currently holds the PSA, 0xFF if unavailable
uint8_t iic_getPSADrawerAddr(uint8_t psaIdx) {
  if (psaIdx >= psaCount)
    return 0xFF;
  uint8_t addr = psaEntries[psaIdx].addr;
  for (uint8_t i = 0; i < drawerCount; i++) {
    for (uint8_t j = 0; j < drawers[i].tinyCount; j++) {
      if (drawers[i].tinyAddrs[j] == addr)
        return drawers[i].addr;
    }
  }
  return 0xFF;
}

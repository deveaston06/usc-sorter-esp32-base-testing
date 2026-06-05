#include <iic_manager.h>

static uint8_t drawerCount = 0;
static uint8_t nextDrawerAddr = 0x10;

// ── PSA list (source of truth on ESP32) ──────────────────────
static uint8_t psaList[PSA_MAX_ENTRIES];
static uint8_t psaCount = 0;

// ── Own UDID ──────────────────────────────────────────────────
static uint8_t udid[UDID_SIZE];

// ── Flags ─────────────────────────────────────────────────────
static volatile bool alertPending = false;
static uint32_t lastScanMs = 0;

// ─────────────────────────────────────────────────────────────
// ALERT ISR
// ─────────────────────────────────────────────────────────────
static void IRAM_ATTR onAlert() { alertPending = true; }

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
// PSA LIST EEPROM PERSISTENCE
// ─────────────────────────────────────────────────────────────
static void psa_save() {
  EEPROM.write(EE_PSA_COUNT, psaCount);
  for (uint8_t i = 0; i < psaCount; i++) {
    EEPROM.write(EE_PSA_LIST + i, psaList[i]);
  }
  EEPROM.commit();
}

static void psa_load() {
  uint8_t count = EEPROM.read(EE_PSA_COUNT);
  psaCount = (count > PSA_MAX_ENTRIES) ? 0 : count;
  for (uint8_t i = 0; i < psaCount; i++) {
    psaList[i] = EEPROM.read(EE_PSA_LIST + i);
  }
}

static bool psa_contains(uint8_t addr) {
  for (uint8_t i = 0; i < psaCount; i++) {
    if (psaList[i] == addr)
      return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────
// SYNC PSA
// Broadcasts full PSA list to every known drawer and to 0x55
// so newly powered but not yet enumerated RP2040s also receive it
// ─────────────────────────────────────────────────────────────
static void syncPSA() {
  // build payload: [CMD][count][addr1]...[addrN]
  uint8_t payload[PSA_MAX_ENTRIES + 2];
  payload[0] = CMD_SYNC_PSA;
  payload[1] = psaCount;
  for (uint8_t i = 0; i < psaCount; i++) {
    payload[i + 2] = psaList[i];
  }
  uint8_t len = psaCount + 2;

  // send to all known drawers individually
  for (uint8_t i = 0; i < drawerCount; i++) {
    Wire.beginTransmission(drawers[i].addr);
    Wire.write(payload, len);
    Wire.endTransmission();
    delay(5);
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

  Wire.beginTransmission(drawerAddr);
  Wire.write(CMD_SCAN_MOD);
  if (Wire.endTransmission() != 0)
    return;

  delay(20);

  uint8_t received =
      Wire.requestFrom(drawerAddr, (uint8_t)(MAX_TINY_PER_DRAWER + 1));
  if (received < 1)
    return;

  uint8_t count = Wire.read();
  if (count > MAX_TINY_PER_DRAWER)
    count = MAX_TINY_PER_DRAWER;

  d->tinyCount = 0;
  for (uint8_t i = 0; i < count && Wire.available(); i++) {
    d->tinyAddrs[d->tinyCount++] = Wire.read();
  }
}

// ─────────────────────────────────────────────────────────────
// SINGLE GET_UDID CYCLE
// ─────────────────────────────────────────────────────────────
static bool getUdidCycle() {
  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  if (Wire.endTransmission() != 0)
    return false;

  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  Wire.write(CMD_GET_UDID);
  if (Wire.endTransmission() != 0)
    return false;

  uint8_t received =
      Wire.requestFrom((uint8_t)ADDR_ARP_DEFAULT, (uint8_t)UDID_SIZE);
  if (received < UDID_SIZE)
    return false;

  uint8_t winnerUdid[UDID_SIZE];
  for (uint8_t i = 0; i < UDID_SIZE; i++) {
    winnerUdid[i] = Wire.read();
  }

  uint8_t newAddr = getNextDrawerAddr();

  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  Wire.write(CMD_ASSIGN_ADDR);
  for (uint8_t i = 0; i < UDID_SIZE; i++)
    Wire.write(winnerUdid[i]);
  Wire.write(newAddr);
  Wire.endTransmission();

  delay(15);

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
  Wire.beginTransmission(ADDR_ARP_DEFAULT);
  Wire.write(CMD_PREPARE_ARP);
  Wire.endTransmission();
  delay(10);

  uint8_t maxCycles = MAX_DRAWERS;
  while (maxCycles-- > 0) {
    if (!getUdidCycle())
      break;
  }

  // push PSA truth to all drawers now that addresses are settled
  syncPSA();

  encoder_requestRedraw();
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
void iic_addPSA(uint8_t tinyAddr) {
  if (psa_contains(tinyAddr) || psaCount >= PSA_MAX_ENTRIES)
    return;
  psaList[psaCount++] = tinyAddr;
  psa_save();
  syncPSA();
}

void iic_removePSA(uint8_t tinyAddr) {
  for (uint8_t i = 0; i < psaCount; i++) {
    if (psaList[i] == tinyAddr) {
      for (uint8_t j = i; j < psaCount - 1; j++) {
        psaList[j] = psaList[j + 1];
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
  attachInterrupt(digitalPinToInterrupt(PIN_ALERT), onAlert, LOW);

  runEnumeration();
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: UPDATE
// ─────────────────────────────────────────────────────────────
void iic_update() {
  if (alertPending) {
    alertPending = false;
    runEnumeration();
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

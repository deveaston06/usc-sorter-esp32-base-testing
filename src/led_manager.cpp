// ─────────────────────────────────────────────────────────────
// led_manager.cpp — ESP32 Base Controller
//
// Responsibilities:
//   - Track LED state per module (RP2040 drawers + ATtiny85 containers)
//   - Maintain drawer→tiny hierarchy for accurate state tracking
//   - Provide status icons for LCD display:
//     'G' = green active
//     'R' = red / default
//     '?' = unknown / never commanded
//   - Auto-update drawer state when all tinies change
// ─────────────────────────────────────────────────────────────

#include <led_manager.h>

// ── Flat state array for fast address lookup ─────────────────
static uint8_t moduleState[ADDR_SPACE];

// ── Drawer structured tracking ───────────────────────────────
static DrawerLEDState drawers[MAX_DRAWERS];
static uint8_t drawerCount = 0;

// ─────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────

static int8_t findDrawerIndex(uint8_t addr) {
  for (uint8_t i = 0; i < drawerCount; i++) {
    if (drawers[i].addr == addr)
      return i;
  }
  return -1;
}

static int8_t findTinyIndex(DrawerLEDState *d, uint8_t tinyAddr) {
  for (uint8_t i = 0; i < d->tinyCount; i++) {
    if (d->tinyAddrs[i] == tinyAddr)
      return i;
  }
  return -1;
}

// Recalculate drawer state based on its ATtiny85s
static void recalcDrawerState(uint8_t drawerIdx) {
  DrawerLEDState *d = &drawers[drawerIdx];

  if (d->tinyCount == 0) {
    // No tinies — drawer keeps its own state
    return;
  }

  // If any tiny is green, drawer shows green
  // If all tinies are red, drawer shows red
  // If all unknown, drawer keeps current state
  bool anyGreen = false;
  bool anyKnown = false;

  for (uint8_t i = 0; i < d->tinyCount; i++) {
    if (d->tinyStates[i] == STATE_GREEN) {
      anyGreen = true;
      anyKnown = true;
      break;
    }
    if (d->tinyStates[i] != STATE_UNKNOWN) {
      anyKnown = true;
    }
  }

  if (anyGreen) {
    d->state = STATE_GREEN;
    moduleState[d->addr] = STATE_GREEN;
  } else if (anyKnown) {
    d->state = STATE_RED;
    moduleState[d->addr] = STATE_RED;
  }
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: DRAWER MANAGEMENT
// ─────────────────────────────────────────────────────────────

void led_registerDrawer(uint8_t addr) {
  if (addr < ADDR_SPACE) {
    int8_t idx = findDrawerIndex(addr);
    if (idx < 0 && drawerCount < MAX_DRAWERS) {
      drawers[drawerCount].addr = addr;
      drawers[drawerCount].state = STATE_UNKNOWN;
      drawers[drawerCount].tinyCount = 0;
      drawerCount++;
    }
    // Initialize flat state as unknown if not set
    if (moduleState[addr] == 0) {
      moduleState[addr] = STATE_UNKNOWN;
    }
  }
}

void led_removeDrawer(uint8_t addr) {
  int8_t idx = findDrawerIndex(addr);
  if (idx >= 0) {
    // Clear all tinies under this drawer from flat state
    for (uint8_t i = 0; i < drawers[idx].tinyCount; i++) {
      moduleState[drawers[idx].tinyAddrs[i]] = STATE_UNKNOWN;
    }
    moduleState[addr] = STATE_UNKNOWN;

    // Compact array
    for (uint8_t i = idx; i < drawerCount - 1; i++) {
      drawers[i] = drawers[i + 1];
    }
    drawerCount--;
  }
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: ATtiny85 MANAGEMENT
// ─────────────────────────────────────────────────────────────

void led_registerTiny(uint8_t drawerAddr, uint8_t tinyAddr) {
  if (tinyAddr >= ADDR_SPACE)
    return;

  int8_t di = findDrawerIndex(drawerAddr);
  if (di < 0)
    return;

  DrawerLEDState *d = &drawers[di];
  int8_t ti = findTinyIndex(d, tinyAddr);

  if (ti < 0 && d->tinyCount < 8) {
    ti = d->tinyCount;
    d->tinyAddrs[ti] = tinyAddr;
    d->tinyStates[ti] = STATE_UNKNOWN;
    d->tinyCount++;
    moduleState[tinyAddr] = STATE_UNKNOWN;
  }
}

void led_removeTiny(uint8_t drawerAddr, uint8_t tinyAddr) {
  int8_t di = findDrawerIndex(drawerAddr);
  if (di < 0)
    return;

  DrawerLEDState *d = &drawers[di];
  int8_t ti = findTinyIndex(d, tinyAddr);

  if (ti >= 0) {
    moduleState[tinyAddr] = STATE_UNKNOWN;

    // Compact
    for (uint8_t i = ti; i < d->tinyCount - 1; i++) {
      d->tinyAddrs[i] = d->tinyAddrs[i + 1];
      d->tinyStates[i] = d->tinyStates[i + 1];
    }
    d->tinyCount--;

    recalcDrawerState(di);
  }
}

void led_clearAllTinies(uint8_t drawerAddr) {
  int8_t di = findDrawerIndex(drawerAddr);
  if (di < 0)
    return;

  DrawerLEDState *d = &drawers[di];
  for (uint8_t i = 0; i < d->tinyCount; i++) {
    moduleState[d->tinyAddrs[i]] = STATE_UNKNOWN;
  }
  d->tinyCount = 0;
  recalcDrawerState(di);
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: SET LED STATE
// ─────────────────────────────────────────────────────────────

void led_setDrawerState(uint8_t addr, bool green) {
  if (addr >= ADDR_SPACE)
    return;

  uint8_t state = green ? STATE_GREEN : STATE_RED;
  moduleState[addr] = state;

  int8_t di = findDrawerIndex(addr);
  if (di >= 0) {
    drawers[di].state = state;
  }
}

void led_setTinyState(uint8_t drawerAddr, uint8_t tinyAddr, bool green) {
  if (tinyAddr >= ADDR_SPACE)
    return;

  uint8_t state = green ? STATE_GREEN : STATE_RED;
  moduleState[tinyAddr] = state;

  int8_t di = findDrawerIndex(drawerAddr);
  if (di >= 0) {
    DrawerLEDState *d = &drawers[di];
    int8_t ti = findTinyIndex(d, tinyAddr);
    if (ti >= 0) {
      d->tinyStates[ti] = state;
    }
    recalcDrawerState(di);
  }
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: QUERY
// ─────────────────────────────────────────────────────────────

uint8_t led_getState(uint8_t addr) {
  if (addr >= ADDR_SPACE)
    return STATE_UNKNOWN;
  return moduleState[addr];
}

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

bool led_isGreen(uint8_t addr) {
  if (addr >= ADDR_SPACE)
    return false;
  return moduleState[addr] == STATE_GREEN;
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: BULK OPERATIONS
// ─────────────────────────────────────────────────────────────

void led_clearAll(void) {
  memset(moduleState, STATE_UNKNOWN, sizeof(moduleState));
  for (uint8_t i = 0; i < drawerCount; i++) {
    drawers[i].state = STATE_UNKNOWN;
    for (uint8_t j = 0; j < drawers[i].tinyCount; j++) {
      drawers[i].tinyStates[j] = STATE_UNKNOWN;
    }
  }
}

void led_setAllRed(uint8_t drawerAddr) {
  int8_t di = findDrawerIndex(drawerAddr);
  if (di < 0)
    return;

  DrawerLEDState *d = &drawers[di];

  // Set drawer to red
  d->state = STATE_RED;
  moduleState[d->addr] = STATE_RED;

  // Set all tinies to red
  for (uint8_t i = 0; i < d->tinyCount; i++) {
    d->tinyStates[i] = STATE_RED;
    moduleState[d->tinyAddrs[i]] = STATE_RED;
  }
}

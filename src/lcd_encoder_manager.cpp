// ─────────────────────────────────────────────────────────────
// lcd_encoder_manager.cpp — ESP32 Base Controller
//
// Menu hierarchy (6 levels):
//
//   LEVEL_TOP
//     [0] Browse Drawers  → LEVEL_DRAWERS
//     [1] PSA Devices     → LEVEL_PSA_LIST
//
//   LEVEL_DRAWERS
//     [0..N] Drawer 0xXX [icon] → LEVEL_CONTAINERS
//
//   LEVEL_CONTAINERS
//     [0..N] ATtiny 0xXX [icon][P] → LEVEL_COMMANDS
//
//   LEVEL_COMMANDS
//     [0] Green LED On
//     [1] Red LED On
//     [2] Add PSA
//     [3] Remove PSA
//     [4] Back
//
//   LEVEL_PSA_LIST
//     [0..N] 0xXX [LED icon] [ON/--] → LEVEL_PSA_COMMANDS
//            ON = device currently enumerated
//            -- = device not currently on bus
//
//   LEVEL_PSA_COMMANDS
//     [0] Green LED On   (sends command if available, shows error if not)
//     [1] Red LED On     (sends command if available, shows error if not)
//     [2] Remove PSA
//     [3] Back
//
// Controls:
//   Rotate    → scroll list
//   Press     → select / execute
//   Hold 1s   → go back one level
// ─────────────────────────────────────────────────────────────

#include <lcd_encoder_manager.h>

// Top level items
#define NUM_TOP_ITEMS 2
static const char *TOP_ITEMS[2] = {"Browse Drawers  ", "PSA Devices     "};

#define NUM_COMMANDS 5
static const char *COMMANDS[5] = {"Green LED On  ", "Red LED On    ",
                                  "Add PSA       ", "Remove PSA    ",
                                  "Back          "};

#define NUM_PSA_COMMANDS 4
static const char *PSA_COMMANDS[NUM_PSA_COMMANDS] = {
    "Green LED On  ", "Red LED On    ", "Remove PSA    ", "Back          "};

// ── LCD instance ──────────────────────────────────────────────
static LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// ── Custom characters ─────────────────────────────────────────
static byte charCursor[8] = {0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10, 0x00};
static byte charArrowU[8] = {0x04, 0x0E, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x00};
static byte charArrowD[8] = {0x04, 0x04, 0x04, 0x04, 0x1F, 0x0E, 0x04, 0x00};

// ── Menu state ────────────────────────────────────────────────
static uint8_t menuLevel = LEVEL_TOP;
static int8_t cursorIdx = 0;
static int8_t scrollOffset = 0;
static uint8_t selectedDrawer = 0; // drawer index in drawers[]
static uint8_t selectedTiny = 0;   // tiny index within selected drawer
static uint8_t selectedPSA = 0;    // PSA index in psaEntries[]
static bool needsRedraw = true;

// ── Encoder state ─────────────────────────────────────────────
static int lastCLK = HIGH;
static int encDelta = 0;

// ── Button state ──────────────────────────────────────────────
static bool lastBtnState = HIGH;
static uint32_t btnPressTime = 0;
static bool btnHoldFired = false;

// ─────────────────────────────────────────────────────────────
// HELPERS — list length for current level
// ─────────────────────────────────────────────────────────────
static uint8_t listLength() {
  switch (menuLevel) {
  case LEVEL_TOP:
    return NUM_TOP_ITEMS;
  case LEVEL_DRAWERS:
    return iic_getDrawerCount();
  case LEVEL_CONTAINERS:
    return iic_getTinyCount(selectedDrawer);
  case LEVEL_COMMANDS:
    return NUM_COMMANDS;
  case LEVEL_PSA_LIST:
    return iic_getPSACount();
  case LEVEL_PSA_COMMANDS:
    return NUM_PSA_COMMANDS;
  default:
    return 0;
  }
}

// ─────────────────────────────────────────────────────────────
// HELPERS — clamp cursor and adjust scroll window
// ─────────────────────────────────────────────────────────────
static void clampCursor() {
  uint8_t len = listLength();
  if (len == 0) {
    cursorIdx = 0;
    scrollOffset = 0;
    return;
  }
  if (cursorIdx < 0)
    cursorIdx = 0;
  if (cursorIdx >= (int8_t)len)
    cursorIdx = (int8_t)len - 1;
  if (cursorIdx < scrollOffset)
    scrollOffset = cursorIdx;
  if (cursorIdx >= scrollOffset + 3)
    scrollOffset = cursorIdx - 2;
}

// ─────────────────────────────────────────────────────────────
// LCD DRAW — header row 0
// ─────────────────────────────────────────────────────────────
static void drawHeader() {
  lcd.setCursor(0, 0);
  char buf[21];

  switch (menuLevel) {
  case LEVEL_TOP:
    lcd.print("Main Menu           ");
    break;

  case LEVEL_DRAWERS:
    lcd.print("Drawers             ");
    break;

  case LEVEL_CONTAINERS:
    snprintf(buf, sizeof(buf), "Drawer 0x%02X         ",
             iic_getDrawerAddr(selectedDrawer));
    lcd.print(buf);
    break;

  case LEVEL_COMMANDS: {
    uint8_t ta = iic_getTinyAddr(selectedDrawer, selectedTiny);
    snprintf(buf, sizeof(buf), "0x%02X %s          ", ta,
             iic_isPSA(ta) ? "[PSA]" : "     ");
    lcd.print(buf);
    break;
  }

  case LEVEL_PSA_LIST:
    lcd.print("PSA Devices         ");
    break;

  case LEVEL_PSA_COMMANDS: {
    uint8_t addr = iic_getPSAAddr(selectedPSA);
    bool avail = iic_isPSAAvailable(selectedPSA);
    snprintf(buf, sizeof(buf), "0x%02X PSA [%s]      ", addr,
             avail ? "ON" : "--");
    lcd.print(buf);
    break;
  }
  }

  // scroll indicators far right col of row 0
  lcd.setCursor(19, 0);
  if (scrollOffset > 0)
    lcd.write(byte(CHAR_ARROW_UP)); // up arrow
  else
    lcd.print(" ");
}

// ─────────────────────────────────────────────────────────────
// LCD DRAW — rows 1-3 (3 visible list items)
// ─────────────────────────────────────────────────────────────
static void drawList() {
  uint8_t len = listLength();

  for (uint8_t row = 0; row < 3; row++) {
    int8_t idx = scrollOffset + row;
    lcd.setCursor(0, row + 1);

    if (idx >= (int8_t)len) {
      lcd.print("                    ");
      continue;
    }

    // cursor
    if (idx == cursorIdx)
      lcd.write(byte(CHAR_ARROW));
    else
      lcd.print(" ");

    lcd.print(" ");

    char buf[18];

    // item label
    switch (menuLevel) {

    case LEVEL_TOP:
      snprintf(buf, sizeof(buf), "%-18s", TOP_ITEMS[idx]);
      lcd.print(buf);
      break;

    case LEVEL_DRAWERS: {
      uint8_t addr = iic_getDrawerAddr(idx);
      char icon = led_getModuleIcon(addr);
      snprintf(buf, sizeof(buf), "0x%02X [%c]          ", addr, icon);
      lcd.print(buf);
      break;
    }

    case LEVEL_CONTAINERS: {
      uint8_t addr = iic_getTinyAddr(selectedDrawer, idx);
      char icon = led_getModuleIcon(addr);
      // show [P] tag if this ATtiny85 is in PSA list
      char psaTag = iic_isPSA(addr) ? 'P' : ' ';
      snprintf(buf, sizeof(buf), "0x%02X [%c][%c]       ", addr, icon, psaTag);
      lcd.print(buf);
      break;
    }

    case LEVEL_COMMANDS:
      snprintf(buf, sizeof(buf), "%-18s", COMMANDS[idx]);
      lcd.print(buf);
      break;

    case LEVEL_PSA_LIST: {
      uint8_t addr = iic_getPSAAddr(idx);
      bool avail = iic_isPSAAvailable(idx);
      char icon = led_getModuleIcon(addr);
      // format: 0xXX [icon] ON  or  0xXX [icon] --
      snprintf(buf, sizeof(buf), "0x%02X [%c] %s        ", addr, icon,
               avail ? "ON " : "-- ");
      lcd.print(buf);
      break;
    }

    case LEVEL_PSA_COMMANDS: {
      // dim LED commands if PSA device is not available
      bool avail = iic_isPSAAvailable(selectedPSA);
      if ((idx == PSA_CMD_IDX_GREEN || idx == PSA_CMD_IDX_RED) && !avail) {
        // show command but with unavailable marker
        snprintf(buf, sizeof(buf), "%-14s[--]", PSA_COMMANDS[idx]);
      } else {
        snprintf(buf, sizeof(buf), "%-18s", PSA_COMMANDS[idx]);
      }
      lcd.print(buf);
      break;
    }
    }
  }

  // down arrow if more items below
  lcd.setCursor(19, 3);
  if (scrollOffset + 3 < (int8_t)len)
    lcd.write(byte(CHAR_ARROW_DOWN));
  else
    lcd.print(" ");
}

// ─────────────────────────────────────────────────────────────
// FULL REDRAW
// ─────────────────────────────────────────────────────────────
static void redraw() {
  drawHeader();
  drawList();
  needsRedraw = false;
}

// ─────────────────────────────────────────────────────────────
// FEEDBACK LINE — temporary message on row 0
// ─────────────────────────────────────────────────────────────
static void showFeedback(const char *msg) {
  lcd.setCursor(0, 0);
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20s", msg);
  lcd.print(buf);
  delay(600);
  needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// EXECUTE SELECTED ITEM
// ─────────────────────────────────────────────────────────────
static void executeSelection() {
  uint8_t len = listLength();
  if (len == 0)
    return;

  switch (menuLevel) {

  case LEVEL_TOP:
    if (cursorIdx == 0) {
      menuLevel = LEVEL_DRAWERS;
      cursorIdx = 0;
      scrollOffset = 0;
      needsRedraw = true;
    } else {
      menuLevel = LEVEL_PSA_LIST;
      cursorIdx = 0;
      scrollOffset = 0;
      needsRedraw = true;
    }
    break;

  case LEVEL_DRAWERS:
    if (iic_getDrawerCount() == 0)
      return;
    selectedDrawer = cursorIdx;
    menuLevel = LEVEL_CONTAINERS;
    cursorIdx = 0;
    scrollOffset = 0;
    needsRedraw = true;
    break;

  case LEVEL_CONTAINERS:
    if (iic_getTinyCount(selectedDrawer) == 0)
      return;
    selectedTiny = cursorIdx;
    menuLevel = LEVEL_COMMANDS;
    cursorIdx = 0;
    scrollOffset = 0;
    needsRedraw = true;
    break;

  case LEVEL_COMMANDS: {
    uint8_t da = iic_getDrawerAddr(selectedDrawer);
    uint8_t ta = iic_getTinyAddr(selectedDrawer, selectedTiny);

    switch (cursorIdx) {
    case CMD_IDX_GREEN:
      iic_sendLedGreen(da, ta);
      showFeedback("Green LED sent      ");
      break;

    case CMD_IDX_RED:
      iic_sendLedRed(da, ta);
      showFeedback("Red LED sent        ");
      break;

    case CMD_IDX_ADD_PSA:
      if (!iic_isPSA(ta)) {
        iic_addPSA(da, ta);
        showFeedback("PSA added + synced  ");
      } else {
        showFeedback("Already in PSA list ");
      }
      break;

    case CMD_IDX_REM_PSA:
      if (iic_isPSA(ta)) {
        iic_removePSA(ta);
        showFeedback("PSA removed + synced");
      } else {
        showFeedback("Not in PSA list     ");
      }
      break;

    case CMD_IDX_BACK:
      menuLevel = LEVEL_CONTAINERS;
      cursorIdx = (int8_t)selectedTiny;
      scrollOffset = 0;
      needsRedraw = true;
      break;
    }
    break;
  }

  // ── PSA LIST ──────────────────────────────────────────────
  case LEVEL_PSA_LIST:
    if (iic_getPSACount() == 0)
      return;
    selectedPSA = cursorIdx;
    menuLevel = LEVEL_PSA_COMMANDS;
    cursorIdx = 0;
    scrollOffset = 0;
    needsRedraw = true;
    break;

  // ── PSA COMMANDS ──────────────────────────────────────────
  case LEVEL_PSA_COMMANDS: {
    uint8_t psaAddr = iic_getPSAAddr(selectedPSA);
    bool available = iic_isPSAAvailable(selectedPSA);
    uint8_t drawerAddr = iic_getPSADrawerAddr(selectedPSA);

    switch (cursorIdx) {
    case PSA_CMD_IDX_GREEN:
      if (available) {
        iic_sendLedGreen(drawerAddr, psaAddr);
        showFeedback("Green LED sent      ");
      } else {
        showFeedback("Device not on bus   ");
      }
      break;

    case PSA_CMD_IDX_RED:
      if (available) {
        iic_sendLedRed(drawerAddr, psaAddr);
        showFeedback("Red LED sent        ");
      } else {
        showFeedback("Device not on bus   ");
      }
      break;

    case PSA_CMD_IDX_REMOVE:
      iic_removePSA(psaAddr);
      showFeedback("PSA removed + synced");
      // go back to PSA list since this entry no longer exists
      menuLevel = LEVEL_PSA_LIST;
      cursorIdx = 0;
      scrollOffset = 0;
      needsRedraw = true;
      break;

    case PSA_CMD_IDX_BACK:
      menuLevel = LEVEL_PSA_LIST;
      cursorIdx = (int8_t)selectedPSA;
      scrollOffset = 0;
      needsRedraw = true;
      break;
    }
    break;
  }
  }
}

// ─────────────────────────────────────────────────────────────
// GO BACK ONE LEVEL (long press)
// ─────────────────────────────────────────────────────────────
static void goBack() {
  switch (menuLevel) {
  case LEVEL_DRAWERS:
  case LEVEL_PSA_LIST:
    menuLevel = LEVEL_TOP;
    cursorIdx = 0;
    scrollOffset = 0;
    break;

  case LEVEL_CONTAINERS:
    menuLevel = LEVEL_DRAWERS;
    cursorIdx = (int8_t)selectedDrawer;
    scrollOffset = 0;
    break;

  case LEVEL_COMMANDS:
    menuLevel = LEVEL_CONTAINERS;
    cursorIdx = (int8_t)selectedTiny;
    scrollOffset = 0;
    break;

  case LEVEL_PSA_COMMANDS:
    menuLevel = LEVEL_PSA_LIST;
    cursorIdx = (int8_t)selectedPSA;
    scrollOffset = 0;
    break;

  case LEVEL_TOP:
  default:
    // already at top, nothing to do
    break;
  }
  needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// ENCODER READ (called every loop)
// ─────────────────────────────────────────────────────────────
static void readEncoder() {
  int clkState = digitalRead(ENC_CLK);
  if (clkState != lastCLK && clkState == LOW) {
    if (digitalRead(ENC_DT) != clkState)
      encDelta++;
    else
      encDelta--;
  }
  lastCLK = clkState;
}

static void handleEncoder() {
  int move = encDelta;
  encDelta = 0;

  if (move == 0)
    return;

  cursorIdx += move;

  clampCursor();
  needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// BUTTON HANDLING (called every loop)
// short press → select
// hold 1s     → back
// ─────────────────────────────────────────────────────────────
static void handleButton() {
  bool btnState = digitalRead(ENC_SW);

  // press detected — record time, reset hold flag
  if (btnState == LOW && lastBtnState == HIGH) {
    btnPressTime = millis();
    btnHoldFired = false;
    delay(DEBOUNCE_MS);
  }

  // held — check if hold threshold reached
  if (btnState == LOW && !btnHoldFired) {
    if (millis() - btnPressTime >= HOLD_MS) {
      btnHoldFired = true;
      goBack();
    }
  }

  // released — execute only if this was a short press, not a hold
  if (btnState == HIGH && lastBtnState == LOW) {
    if (!btnHoldFired) {
      executeSelection();
    }
    delay(DEBOUNCE_MS);
  }

  lastBtnState = btnState;
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: INIT
// ─────────────────────────────────────────────────────────────
void lcd_encoder_init() {
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.createChar(CHAR_ARROW, charCursor);
  lcd.createChar(CHAR_ARROW_UP, charArrowU);
  lcd.createChar(CHAR_ARROW_DOWN, charArrowD);
  lcd.clear();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  lastCLK = digitalRead(ENC_CLK);

  menuLevel = LEVEL_TOP;
  needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: UPDATE
// ─────────────────────────────────────────────────────────────
void encoder_update() {
  readEncoder();
  handleEncoder();
  handleButton();

  if (needsRedraw)
    redraw();
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: FORCE REDRAW
// ─────────────────────────────────────────────────────────────
void encoder_requestRedraw() { needsRedraw = true; }

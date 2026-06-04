#include <lcd_manager.h>

static const char *COMMANDS[NUM_COMMANDS] = {"Green LED On  ", "Red LED On    ",
                                             "Add PSA       ", "Remove PSA    ",
                                             "Back          "};

// ── LCD instance ──────────────────────────────────────────────
static LiquidCrystal lcd(LCD_RS, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

// ── Custom characters ─────────────────────────────────────────
static byte charCursor[8] = {0x10, 0x18, 0x1C, 0x1E, 0x1C, 0x18, 0x10, 0x00};
static byte charArrowU[8] = {0x04, 0x0E, 0x1F, 0x04, 0x04, 0x04, 0x04, 0x00};
static byte charArrowD[8] = {0x04, 0x04, 0x04, 0x04, 0x1F, 0x0E, 0x04, 0x00};

// ── Menu state ────────────────────────────────────────────────
static uint8_t menuLevel = LEVEL_DRAWERS;
static int8_t cursorIdx = 0;
static int8_t scrollOffset = 0;
static uint8_t selectedDrawer = 0; // drawerIdx selected at level 0
static uint8_t selectedTiny = 0;   // tinyIdx selected at level 1
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
  case LEVEL_DRAWERS:
    return iic_getDrawerCount();
  case LEVEL_CONTAINERS:
    return iic_getTinyCount(selectedDrawer);
  case LEVEL_COMMANDS:
    return NUM_COMMANDS;
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
  switch (menuLevel) {
  case LEVEL_DRAWERS:
    lcd.print("Drawers             ");
    break;
  case LEVEL_CONTAINERS: {
    char buf[21];
    snprintf(buf, sizeof(buf), "Drawer 0x%02X         ",
             iic_getDrawerAddr(selectedDrawer));
    lcd.print(buf);
    break;
  }
  case LEVEL_COMMANDS: {
    uint8_t ta = iic_getTinyAddr(selectedDrawer, selectedTiny);
    char tag[6] = "     ";
    if (iic_isPSA(ta))
      strncpy(tag, "[PSA]", 5);
    char buf[21];
    snprintf(buf, sizeof(buf), "0x%02X %s          ", ta, tag);
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

    // item label
    switch (menuLevel) {
    case LEVEL_DRAWERS: {
      uint8_t addr = iic_getDrawerAddr(idx);
      char icon = led_getModuleIcon(addr);
      char buf[18];
      snprintf(buf, sizeof(buf), "0x%02X [%c]          ", addr, icon);
      lcd.print(buf);
      break;
    }
    case LEVEL_CONTAINERS: {
      uint8_t addr = iic_getTinyAddr(selectedDrawer, idx);
      char icon = led_getModuleIcon(addr);
      // show [P] tag if this ATtiny85 is in PSA list
      char psaTag = iic_isPSA(addr) ? 'P' : ' ';
      char buf[18];
      snprintf(buf, sizeof(buf), "0x%02X [%c][%c]       ", addr, icon, psaTag);
      lcd.print(buf);
      break;
    }
    case LEVEL_COMMANDS: {
      char buf[18];
      snprintf(buf, sizeof(buf), "%-18s", COMMANDS[idx]);
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
        iic_addPSA(ta);
        showFeedback("PSA added + synced  ");
      } else {
        showFeedback("Already in PSA list ");
      }
      break;

    case CMD_IDX_REMOVE_PSA:
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
  }
}

// ─────────────────────────────────────────────────────────────
// GO BACK ONE LEVEL (long press)
// ─────────────────────────────────────────────────────────────
static void goBack() {
  if (menuLevel == LEVEL_COMMANDS) {
    menuLevel = LEVEL_CONTAINERS;
    cursorIdx = (int8_t)selectedTiny;
    scrollOffset = 0;
  } else if (menuLevel == LEVEL_CONTAINERS) {
    menuLevel = LEVEL_DRAWERS;
    cursorIdx = (int8_t)selectedDrawer;
    scrollOffset = 0;
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
  if (encDelta == 0)
    return;
  cursorIdx += encDelta;
  encDelta = 0;
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

  if (btnState == LOW && lastBtnState == HIGH) {
    btnPressTime = millis();
    btnHoldFired = false;
    delay(DEBOUNCE_MS);
  }

  if (btnState == LOW && !btnHoldFired) {
    if (millis() - btnPressTime >= HOLD_MS) {
      btnHoldFired = true;
      goBack();
    }
  }

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
void encoder_init() {
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.createChar(CHAR_ARROW, charCursor);
  lcd.createChar(CHAR_ARROW_UP, charArrowU);
  lcd.createChar(CHAR_ARROW_DOWN, charArrowD);
  lcd.clear();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  lastCLK = digitalRead(ENC_CLK);

  needsRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: UPDATE (call every loop iteration)
// ─────────────────────────────────────────────────────────────
void encoder_update() {
  readEncoder();
  handleEncoder();
  handleButton();

  if (needsRedraw)
    redraw();
}

// ─────────────────────────────────────────────────────────────
// PUBLIC: FORCE REDRAW (call from iic_update after scan)
// ─────────────────────────────────────────────────────────────
void encoder_requestRedraw() { needsRedraw = true; }

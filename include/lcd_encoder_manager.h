#ifndef LCD_MANAGER_H
#define LCD_MANAGER_H

#include <Arduino.h>
#include <LiquidCrystal.h>
#include <iic_manager.h>
#include <led_manager.h>

// ── LCD pins ──────────────────────────────────────────────────
#define LCD_RS 32
#define LCD_EN 13
#define LCD_D4 33
#define LCD_D5 25
#define LCD_D6 26
#define LCD_D7 27
#define LCD_COLS 20
#define LCD_ROWS 4

// ── Encoder pins ──────────────────────────────────────────────
#define ENC_CLK 19
#define ENC_DT 18
#define ENC_SW 17

// ── Timing ────────────────────────────────────────────────────
#define DEBOUNCE_MS 50
#define HOLD_MS 1000

// ── Menu levels ───────────────────────────────────────────────
#define LEVEL_DRAWERS 0
#define LEVEL_CONTAINERS 1
#define LEVEL_COMMANDS 2
#define NUM_COMMANDS 5

// Command indices
#define CMD_IDX_GREEN 0
#define CMD_IDX_RED 1
#define CMD_IDX_ADD_PSA 2
#define CMD_IDX_REMOVE_PSA 3
#define CMD_IDX_BACK 4

// Custom Characters
#define CHAR_ARROW 0
#define CHAR_ARROW_UP 1
#define CHAR_ARROW_DOWN 2

void lcd_encoder_init();
void encoder_update();
void encoder_requestRedraw();

#endif // !LCD_MANAGER_H

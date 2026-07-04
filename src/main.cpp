#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
// The FreeSans*/FreeMono* fonts are already pulled in via LOAD_GFXFF in TFT_eSPI.h.
// Antonio Bold (condensed) for the LCARS design, generated from the TTF.
#include "fonts_antonio.h"
#ifndef SIMULATE_SENSOR
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#endif

// On the CYD the XPT2046 touch controller sits on its own SPI bus,
// separate from the display (see PINS.md in the CYD repo).
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ 36

// The XPT2046 reports raw values (roughly 200-3900). Distinguishing the
// left from the right half of the display only needs the range midpoint;
// no real calibration is required for that.
#define TOUCH_RAW_MID 2048

// The CYD's microSD slot. It shares SPI usage with the touch controller:
// the bus is re-pointed to the SD pins for each card access and back to
// the touch pins afterwards (see beginSd/endSd).
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SETTINGS_PATH "/settings.txt"

// Onboard I2C connector of the CYD (JST connector CN1: GND, IO22, IO27, 3V3),
// used for the BME680 sensor. See README, "Connecting the BME680".
#define I2C_SDA 27
#define I2C_SCL 22

// Rough air-quality classification based on gas resistance (kOhm).
// Not a calibrated IAQ index (that would require the Bosch BSEC library),
// but good enough for a coarse good/medium/poor indicator. These are the
// defaults; they can be overridden via the SD card settings file.
#define GAS_GOOD_KOHM 50.0f
#define GAS_MODERATE_KOHM 20.0f

// Default measurement interval; overridable via the SD card settings file.
#define MEASURE_INTERVAL_MS 2000

// Selectable dashboard designs. Tapping the touchscreen switches between
// them; the selection is stored in NVS and survives reboots.
enum : uint8_t { DESIGN_LCARS = 0, DESIGN_TILES, DESIGN_TERM, DESIGN_BAUHAUS, DESIGN_COUNT };
const char *DESIGN_NAMES[DESIGN_COUNT] = {"LCARS", "TILES", "TERMINAL", "BAUHAUS"};

// Runtime settings with their defaults. A settings.txt on the microSD card
// overrides them at boot and is rewritten whenever a setting changes on the
// device; without a card the design keeps persisting in NVS only.
struct Settings {
  uint8_t design = DESIGN_LCARS;
  uint32_t intervalMs = MEASURE_INTERVAL_MS;
  bool touchSwap = false; // swaps the left/right tap direction
  float gasGoodKohm = GAS_GOOD_KOHM;
  float gasModerateKohm = GAS_MODERATE_KOHM;
};
Settings settings;
bool sdAvailable = false;

struct Readings {
  float tempC;
  float humidity;
  float pressureHpa;
  float gasKOhm;
};

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
#ifndef SIMULATE_SENSOR
Adafruit_BME680 bme;
#endif
Preferences prefs;

uint8_t currentDesign = DESIGN_LCARS;
Readings lastReading;
bool hasReading = false;
unsigned long lastMeasurement = 0;

// Color palette; values are filled once in setup() via color565.
// LCARS (Star Trek computer interface): black background with warm
// orange/lilac tones. Plus colors for the tiles and terminal designs.
uint16_t COL_BG, COL_ORANGE, COL_LILAC, COL_PEACH, COL_PALEBLUE, COL_TAN,
    COL_BLACK_TEXT, COL_MUTED, COL_READOUT, COL_COOL, COL_GOOD, COL_WARN,
    COL_AQ_GOOD, COL_AQ_MID, COL_AQ_BAD,
    COL_NIGHT, COL_TILE, COL_TILE_TEXT,
    COL_TERM_BRIGHT, COL_TERM_DIM,
    COL_BAU_BG, COL_BAU_RED, COL_BAU_BLUE, COL_BAU_YELLOW;

// ---------------------------------------------------------------------------
// Settings on the microSD card
// ---------------------------------------------------------------------------

// Points the shared SPI bus at the SD card. Note there is deliberately no
// SPIClass::end() anywhere: re-running begin() only re-routes the pins via
// the GPIO matrix (ending the bus could disturb other peripherals), and the
// idle chip-select lines keep the momentarily unused device quiet.
bool beginSd() {
  touchSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, touchSpi)) {
    touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    return false;
  }
  return true;
}

// Releases the card and routes the SPI bus back to the touch controller.
void endSd() {
  SD.end();
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
}

uint8_t designFromName(String name) {
  name.trim();
  for (uint8_t i = 0; i < DESIGN_COUNT; i++) {
    if (name.equalsIgnoreCase(DESIGN_NAMES[i])) return i;
  }
  return DESIGN_COUNT; // not a valid design name
}

// Applies one key=value pair; unknown keys and out-of-range values are
// ignored so a hand-edited file cannot break the dashboard.
void applySetting(const String &key, const String &value) {
  if (key == "design") {
    uint8_t d = designFromName(value);
    if (d < DESIGN_COUNT) settings.design = d;
  } else if (key == "interval_ms") {
    long v = value.toInt();
    if (v >= 500 && v <= 3600000L) settings.intervalMs = (uint32_t)v;
  } else if (key == "touch_swap") {
    settings.touchSwap = value.toInt() != 0;
  } else if (key == "gas_good_kohm") {
    float v = value.toFloat();
    if (v > 0) settings.gasGoodKohm = v;
  } else if (key == "gas_moderate_kohm") {
    float v = value.toFloat();
    if (v > 0) settings.gasModerateKohm = v;
  }
}

// Reads SETTINGS_PATH from the SD card. Returns true if the file existed
// and was read; sets sdAvailable when a card is mounted.
bool loadSettingsFromSd() {
  if (!beginSd()) {
    Serial.println("SD: no card detected, settings persist in NVS only");
    return false;
  }
  sdAvailable = true;
  File f = SD.open(SETTINGS_PATH, FILE_READ);
  if (!f) {
    endSd();
    Serial.println("SD: no settings file yet");
    return false;
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    value.trim();
    applySetting(key, value);
  }
  f.close();
  endSd();
  Serial.println("SD: settings loaded");
  return true;
}

// Writes the current settings back to the card as a full rewrite, keeping
// the file self-documenting. Returns false if no card is available.
bool saveSettingsToSd() {
  if (!sdAvailable || !beginSd()) return false;
  File f = SD.open(SETTINGS_PATH, FILE_WRITE); // "w" mode: truncates
  if (!f) {
    endSd();
    return false;
  }
  String designName = DESIGN_NAMES[settings.design];
  designName.toLowerCase();
  f.println("# CYD room climate settings");
  f.println("# design: lcars | tiles | terminal | bauhaus");
  f.println("# interval_ms: measurement interval in ms (500-3600000)");
  f.println("# touch_swap: 1 swaps the left/right tap direction");
  f.println("# gas_*_kohm: air-quality thresholds (gas resistance)");
  f.printf("design=%s\n", designName.c_str());
  f.printf("interval_ms=%lu\n", (unsigned long)settings.intervalMs);
  f.printf("touch_swap=%d\n", settings.touchSwap ? 1 : 0);
  f.printf("gas_good_kohm=%.1f\n", settings.gasGoodKohm);
  f.printf("gas_moderate_kohm=%.1f\n", settings.gasModerateKohm);
  f.close();
  endSd();
  return true;
}

// ---------------------------------------------------------------------------
// Shared helpers (status colors, air quality, degree symbol)
// ---------------------------------------------------------------------------

uint16_t temperatureColor(float tempC) {
  if (tempC < 18.0f) return COL_COOL;
  if (tempC <= 27.0f) return COL_GOOD;
  return COL_WARN;
}

uint16_t humidityColor(float humidity) {
  if (humidity < 30.0f) return COL_WARN;
  if (humidity <= 60.0f) return COL_GOOD;
  return COL_COOL;
}

const char *airQualityLabel(float gasKOhm, uint16_t &color) {
  if (gasKOhm >= settings.gasGoodKohm) {
    color = COL_AQ_GOOD;
    return "GUT";
  }
  if (gasKOhm >= settings.gasModerateKohm) {
    color = COL_AQ_MID;
    return "MITTEL";
  }
  color = COL_AQ_BAD;
  return "SCHLECHT";
}

// A bare "C" would be the wrong unit - "C" is not "degrees Celsius".
// The GFX fonts only cover ASCII 0x20-0x7E and have no degree sign, so it
// is drawn by hand as a small raised circle in front of the "C".
// anchorX/anchorY: MR anchor (right-aligned, vertically centered) for the "C".
void drawDegreeCSuffix(int16_t anchorX, int16_t anchorY, uint16_t color, uint16_t bg,
                       const GFXfont *font) {
  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(font);
  tft.setTextColor(color, bg);
  tft.drawString("C", anchorX, anchorY);
  int16_t textW = tft.textWidth("C");
  tft.drawCircle(anchorX - textW - 6, anchorY - 7, 2, color);
}

// ---------------------------------------------------------------------------
// Design 1: LCARS
// ---------------------------------------------------------------------------

#define ELBOW_SIZE 56
#define ROW_ENDCAP_W 72

struct Row {
  int16_t x, y, w, h;
};

// Four LCARS readout rows in the content area right of the sidebar.
Row rowTemp = {64, 68, 252, 36};
Row rowHum = {64, 112, 252, 36};
Row rowPress = {64, 156, 252, 36};
Row rowAQ = {64, 200, 252, 36};

// Starfleet delta drawn as a silhouette, modeled on the original insignia.
// The outline is built row by row from two curves: the outer flank sweeps
// from the narrow tip out to the wing tips (x grows with exponent 1.35,
// keeping the top slender and the bottom flared), and from ~60% height the
// inner concave notch cuts in (exponent 0.45, giving a wide, shallow
// rounding). Row-wise filling keeps the thin trailing wings free of gaps.
void drawStarfleetDelta(int16_t cx, int16_t top, int16_t w, int16_t h, uint16_t color) {
  float halfW = w / 2.0f;
  float notchY = 0.60f * h;
  for (int16_t y = 0; y <= h; y++) {
    float fy = (float)y / h;
    int16_t xo = (int16_t)roundf(halfW * powf(fy, 1.35f));
    int16_t xi = 0;
    if (y > notchY) {
      xi = (int16_t)(halfW * powf((y - notchY) / (h - notchY), 0.45f));
    }
    if (xo < xi) continue;
    tft.drawFastHLine(cx - xo, top + y, xo - xi + 1, color);
    tft.drawFastHLine(cx + xi, top + y, xo - xi + 1, color);
  }
}

// Draws a rectangle that is rounded on the left side only (LCARS end cap):
// first a full capsule shape, then the right half is painted over square.
void drawLeftCap(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  int16_t r = h / 2;
  tft.fillRoundRect(x, y, w, h, r, color);
  if (w > r) {
    tft.fillRect(x + r, y, w - r, h, color);
  }
}

void clearRowPanel(const Row &r) {
  tft.fillRect(r.x + ROW_ENDCAP_W, r.y, r.w - ROW_ENDCAP_W, r.h, COL_BG);
}

void drawRowEndcap(const Row &r, const char *label, uint16_t color) {
  drawLeftCap(r.x, r.y, ROW_ENDCAP_W, r.h, color);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&Antonio_Bold9pt7b);
  // Transparent instead of opaque: an overly wide label would otherwise
  // spill a colored rectangle over the dark value area to its right.
  tft.setTextColor(COL_BLACK_TEXT);
  tft.drawString(label, r.x + ROW_ENDCAP_W / 2, r.y + r.h / 2 + 1);
}

void drawRowValue(const Row &r, const char *value, uint16_t color, const GFXfont *font) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(font);
  tft.setTextColor(color, COL_BG);
  tft.drawString(value, r.x + ROW_ENDCAP_W + 10, r.y + r.h / 2 + 1);
}

void drawRowSuffix(const Row &r, const char *text) {
  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(&Antonio_Bold9pt7b);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString(text, r.x + r.w - 8, r.y + r.h / 2 + 1);
}

void drawTempSuffix(const Row &r) {
  drawDegreeCSuffix(r.x + r.w - 8, r.y + r.h / 2 + 1, COL_MUTED, COL_BG,
                    &Antonio_Bold9pt7b);
}

void drawSidebarChip(int16_t y, const char *code, uint16_t color) {
  tft.fillRoundRect(12, y, 40, 16, 8, color);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&Antonio_Bold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, color);
  tft.drawString(code, 32, y + 9);
}

void drawStaticLcars() {
  tft.fillScreen(COL_BG);

  // Elbow: the classic LCARS corner sweep, top left.
  tft.fillRoundRect(4, 4, ELBOW_SIZE, ELBOW_SIZE, 18, COL_ORANGE);

  // Starfleet delta as a black silhouette inside the elbow.
  drawStarfleetDelta(32, 9, 36, 44, COL_BLACK_TEXT);

  // Header bar to the right of the elbow.
  tft.fillRoundRect(64, 4, 252, 26, 13, COL_ORANGE);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&Antonio_Bold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_ORANGE);
  tft.drawString("RAUMKLIMA", 76, 17);
  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(&Antonio_Bold9pt7b);
  tft.drawString("NCC-2432", 308, 17);

  // Sidebar below the elbow.
  tft.fillRoundRect(4, 64, ELBOW_SIZE, 172, 18, COL_LILAC);
  drawSidebarChip(90, "07", COL_BG);
  drawSidebarChip(140, "41", COL_BG);
#ifdef SIMULATE_SENSOR
  drawSidebarChip(190, "SIM", COL_WARN);
#else
  drawSidebarChip(190, "LIVE", COL_GOOD);
#endif

  drawRowEndcap(rowTemp, "TEMP", COL_PEACH);
  drawRowEndcap(rowHum, "FEUCHT", COL_PALEBLUE);
  drawRowEndcap(rowPress, "DRUCK", COL_TAN);
  drawRowEndcap(rowAQ, "LUFTQ", COL_LILAC);

  drawTempSuffix(rowTemp);
  drawRowSuffix(rowHum, "%");
  drawRowSuffix(rowPress, "hPa");
}

void drawReadingsLcars(const Readings &rd) {
  char buf[16];

  clearRowPanel(rowTemp);
  snprintf(buf, sizeof(buf), "%.1f", rd.tempC);
  drawRowValue(rowTemp, buf, temperatureColor(rd.tempC), &Antonio_Bold18pt7b);
  drawTempSuffix(rowTemp);

  clearRowPanel(rowHum);
  snprintf(buf, sizeof(buf), "%.0f", rd.humidity);
  drawRowValue(rowHum, buf, humidityColor(rd.humidity), &Antonio_Bold18pt7b);
  drawRowSuffix(rowHum, "%");

  clearRowPanel(rowPress);
  snprintf(buf, sizeof(buf), "%.0f", rd.pressureHpa);
  drawRowValue(rowPress, buf, COL_READOUT, &Antonio_Bold18pt7b);
  drawRowSuffix(rowPress, "hPa");

  uint16_t aqColor;
  const char *aqLabel = airQualityLabel(rd.gasKOhm, aqColor);
  drawRowEndcap(rowAQ, "LUFTQ", aqColor);
  clearRowPanel(rowAQ);
  drawRowValue(rowAQ, aqLabel, aqColor, &Antonio_Bold12pt7b);
  snprintf(buf, sizeof(buf), "%.0f KOHM", rd.gasKOhm);
  drawRowSuffix(rowAQ, buf);
}

// ---------------------------------------------------------------------------
// Design 2: Tiles - dark 2x2 grid with large values
// ---------------------------------------------------------------------------

struct Tile {
  int16_t x, y, w, h;
};

Tile tileTemp = {8, 34, 148, 96};
Tile tileHum = {164, 34, 148, 96};
Tile tilePress = {8, 138, 148, 96};
Tile tileAQ = {164, 138, 148, 96};

void drawTileFrame(const Tile &t, const char *label) {
  tft.fillRoundRect(t.x, t.y, t.w, t.h, 10, COL_TILE);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_MUTED, COL_TILE);
  tft.drawString(label, t.x + 10, t.y + 8);
}

// Clears only the value zone in the middle of the tile; the label (top)
// and the unit (bottom right) stay in place.
void drawTileValue(const Tile &t, const char *value, uint16_t color, const GFXfont *font) {
  tft.fillRect(t.x + 6, t.y + 26, t.w - 12, 44, COL_TILE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(font);
  tft.setTextColor(color, COL_TILE);
  tft.drawString(value, t.x + t.w / 2, t.y + 48);
}

void drawTileUnit(const Tile &t, const char *unit) {
  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_MUTED, COL_TILE);
  tft.drawString(unit, t.x + t.w - 10, t.y + t.h - 14);
}

void drawStaticTiles() {
  tft.fillScreen(COL_NIGHT);

  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_TILE_TEXT, COL_NIGHT);
  tft.drawString("RAUMKLIMA", 10, 16);

#ifdef SIMULATE_SENSOR
  tft.fillRoundRect(262, 6, 50, 20, 10, COL_WARN);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_WARN);
  tft.drawString("SIM", 287, 17);
#else
  tft.fillRoundRect(262, 6, 50, 20, 10, COL_GOOD);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_GOOD);
  tft.drawString("LIVE", 287, 17);
#endif

  drawTileFrame(tileTemp, "TEMPERATUR");
  drawTileFrame(tileHum, "FEUCHTE");
  drawTileFrame(tilePress, "DRUCK");
  drawTileFrame(tileAQ, "LUFTGUETE");

  drawDegreeCSuffix(tileTemp.x + tileTemp.w - 10, tileTemp.y + tileTemp.h - 14,
                    COL_MUTED, COL_TILE, &FreeSansBold9pt7b);
  drawTileUnit(tileHum, "%");
  drawTileUnit(tilePress, "hPa");
}

void drawReadingsTiles(const Readings &rd) {
  char buf[16];

  snprintf(buf, sizeof(buf), "%.1f", rd.tempC);
  drawTileValue(tileTemp, buf, temperatureColor(rd.tempC), &FreeSansBold18pt7b);

  snprintf(buf, sizeof(buf), "%.0f", rd.humidity);
  drawTileValue(tileHum, buf, humidityColor(rd.humidity), &FreeSansBold18pt7b);

  snprintf(buf, sizeof(buf), "%.0f", rd.pressureHpa);
  drawTileValue(tilePress, buf, COL_READOUT, &FreeSansBold18pt7b);

  uint16_t aqColor;
  const char *aqLabel = airQualityLabel(rd.gasKOhm, aqColor);
  drawTileValue(tileAQ, aqLabel, aqColor, &FreeSansBold12pt7b);
  // The gas resistance is dynamic, so it gets its own clear zone bottom right.
  tft.fillRect(tileAQ.x + 6, tileAQ.y + tileAQ.h - 26, tileAQ.w - 12, 20, COL_TILE);
  snprintf(buf, sizeof(buf), "%.0f kOhm", rd.gasKOhm);
  drawTileUnit(tileAQ, buf);
}

// ---------------------------------------------------------------------------
// Design 3: Terminal - monochrome green retro console
// ---------------------------------------------------------------------------

#define TERM_VALUE_X 145
#define TERM_ROW_TEMP 48
#define TERM_ROW_HUM 80
#define TERM_ROW_PRESS 112
#define TERM_ROW_AQ 144
#define TERM_ROW_GAS 176

void drawTermLabel(int16_t y, const char *label) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeMonoBold12pt7b);
  tft.setTextColor(COL_TERM_DIM, COL_BG);
  tft.drawString(label, 10, y);
}

void drawTermValue(int16_t y, const char *value, uint16_t color) {
  tft.fillRect(TERM_VALUE_X, y - 14, 320 - TERM_VALUE_X, 28, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeMonoBold12pt7b);
  tft.setTextColor(color, COL_BG);
  tft.drawString(value, TERM_VALUE_X, y);
}

void drawStaticTerm() {
  tft.fillScreen(COL_BG);

  // Inverted header line like on an old data terminal.
  tft.fillRect(0, 0, 320, 24, COL_TERM_BRIGHT);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeMonoBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_TERM_BRIGHT);
  tft.drawString("CYD RAUMKLIMA-KONSOLE", 8, 12);

  drawTermLabel(TERM_ROW_TEMP, "TEMP....:");
  drawTermLabel(TERM_ROW_HUM, "FEUCHT..:");
  drawTermLabel(TERM_ROW_PRESS, "DRUCK...:");
  drawTermLabel(TERM_ROW_AQ, "LUFTQ...:");
  drawTermLabel(TERM_ROW_GAS, "GAS.....:");

  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeMonoBold9pt7b);
  tft.setTextColor(COL_TERM_DIM, COL_BG);
#ifdef SIMULATE_SENSOR
  tft.drawString("> [SIM] L=ZURUECK R=VOR", 10, 222);
#else
  tft.drawString("> [LIVE] L=ZURUECK R=VOR", 10, 222);
#endif
}

void drawReadingsTerm(const Readings &rd) {
  char buf[20];

  // Temperature: value, then degree circle + "C" by hand (fonts are ASCII-only).
  snprintf(buf, sizeof(buf), "%.1f", rd.tempC);
  drawTermValue(TERM_ROW_TEMP, buf, COL_TERM_BRIGHT);
  int16_t w = tft.textWidth(buf);
  tft.drawCircle(TERM_VALUE_X + w + 8, TERM_ROW_TEMP - 8, 2, COL_TERM_BRIGHT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("C", TERM_VALUE_X + w + 14, TERM_ROW_TEMP);

  snprintf(buf, sizeof(buf), "%.0f %%", rd.humidity);
  drawTermValue(TERM_ROW_HUM, buf, COL_TERM_BRIGHT);

  snprintf(buf, sizeof(buf), "%.0f hPa", rd.pressureHpa);
  drawTermValue(TERM_ROW_PRESS, buf, COL_TERM_BRIGHT);

  uint16_t aqColor;
  const char *aqLabel = airQualityLabel(rd.gasKOhm, aqColor);
  drawTermValue(TERM_ROW_AQ, aqLabel, aqColor);

  snprintf(buf, sizeof(buf), "%.0f kOhm", rd.gasKOhm);
  drawTermValue(TERM_ROW_GAS, buf, COL_TERM_BRIGHT);
}

// ---------------------------------------------------------------------------
// Design 4: Bauhaus - primary-colored basic shapes on paper white,
// separated by bold black grid lines. Shape/color mapping after
// Kandinsky: circle=blue, square=red, triangle=yellow.
// ---------------------------------------------------------------------------

struct BauCell {
  int16_t x, y, w, h;
};

// 2x2 grid; the gaps in between are filled by the 4px grid lines.
BauCell bauTemp = {0, 40, 158, 96};
BauCell bauHum = {162, 40, 158, 96};
BauCell bauPress = {0, 140, 158, 100};
BauCell bauAQ = {162, 140, 158, 100};

// Air-quality status color from the fixed primary palette:
// blue=good, yellow=medium, red=poor.
uint16_t bauhausAqColor(float gasKOhm) {
  if (gasKOhm >= settings.gasGoodKohm) return COL_BAU_BLUE;
  if (gasKOhm >= settings.gasModerateKohm) return COL_BAU_YELLOW;
  return COL_BAU_RED;
}

// Half circle with the flat side down - the fourth element of the shape
// canon, distinct from circle, square and triangle. Drawn as a full circle
// whose lower half is painted over with the background (like drawLeftCap).
void drawBauHalfCircle(int16_t cx, int16_t baseY, int16_t r, uint16_t color) {
  tft.fillCircle(cx, baseY, r, color);
  tft.fillRect(cx - r, baseY + 1, 2 * r + 1, r + 1, COL_BAU_BG);
}

void drawBauLabel(const BauCell &c, const char *label) {
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(label, c.x + 40, c.y + 12);
}

// Clears the value zone below shape and label.
void clearBauValue(const BauCell &c) {
  tft.fillRect(c.x + 4, c.y + 34, c.w - 8, 58, COL_BAU_BG);
}

void drawBauValue(const BauCell &c, const char *value, const GFXfont *font) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(font);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(value, c.x + 12, c.y + 62);
}

// Small unit behind the large reading; the y position is chosen so the
// baselines of the 24pt value and the 9pt unit line up. xStart is the
// offset from the left cell edge.
void drawBauUnit(const BauCell &c, int16_t xStart, const char *unit) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(unit, c.x + xStart, c.y + 72);
}

void drawStaticBauhaus() {
  tft.fillScreen(COL_BAU_BG);

  // Header: title, the three basic shapes, SIM/LIVE marker.
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString("RAUMKLIMA", 10, 18);

  // Circle, square, triangle; the fourth slot to their right belongs to
  // the air-quality status half circle, drawn dynamically in
  // drawReadingsBauhaus.
  tft.fillCircle(186, 18, 8, COL_BAU_BLUE);
  tft.fillRect(202, 10, 16, 16, COL_BAU_RED);
  tft.fillTriangle(226, 26, 242, 26, 234, 10, COL_BAU_YELLOW);

  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
#ifdef SIMULATE_SENSOR
  tft.drawString("SIM", 312, 19);
#else
  tft.drawString("LIVE", 312, 19);
#endif

  // Grid lines.
  tft.fillRect(0, 36, 320, 4, COL_BLACK_TEXT);
  tft.fillRect(0, 136, 320, 4, COL_BLACK_TEXT);
  tft.fillRect(158, 40, 4, 200, COL_BLACK_TEXT);

  // Shapes and labels of the cells; the status half circle of the
  // air-quality cell is drawn dynamically in drawReadingsBauhaus.
  // Short labels so they fit next to the shapes at 12pt.
  tft.fillRect(bauTemp.x + 10, bauTemp.y + 10, 24, 24, COL_BAU_RED);
  drawBauLabel(bauTemp, "TEMP");

  tft.fillCircle(bauHum.x + 22, bauHum.y + 22, 12, COL_BAU_BLUE);
  drawBauLabel(bauHum, "FEUCHTE");

  tft.fillTriangle(bauPress.x + 10, bauPress.y + 34, bauPress.x + 34, bauPress.y + 34,
                   bauPress.x + 22, bauPress.y + 10, COL_BAU_YELLOW);
  drawBauLabel(bauPress, "DRUCK");

  drawBauLabel(bauAQ, "LUFTQ");
}

void drawReadingsBauhaus(const Readings &rd) {
  char buf[16];
  int16_t vw;

  clearBauValue(bauTemp);
  snprintf(buf, sizeof(buf), "%.1f", rd.tempC);
  drawBauValue(bauTemp, buf, &FreeSansBold24pt7b);
  vw = tft.textWidth(buf); // font is still the 24pt value font
  // Degree circle by hand in front of the "C" (fonts are ASCII-only).
  tft.drawCircle(bauTemp.x + 12 + vw + 9, bauTemp.y + 65, 2, COL_BLACK_TEXT);
  drawBauUnit(bauTemp, 12 + vw + 14, "C");

  clearBauValue(bauHum);
  snprintf(buf, sizeof(buf), "%.0f", rd.humidity);
  drawBauValue(bauHum, buf, &FreeSansBold24pt7b);
  drawBauUnit(bauHum, 12 + tft.textWidth(buf) + 8, "%");

  clearBauValue(bauPress);
  snprintf(buf, sizeof(buf), "%.0f", rd.pressureHpa);
  drawBauValue(bauPress, buf, &FreeSansBold24pt7b);
  drawBauUnit(bauPress, 12 + tft.textWidth(buf) + 8, "hPa");

  // The air-quality status half circle changes color with the reading:
  // once large in the cell, once small in the header as the fourth element
  // next to the three basic shapes. The small one uses r=8 so the helper's
  // paint-over zone stays above the grid line at y=36.
  uint16_t aqCol = bauhausAqColor(rd.gasKOhm);
  tft.fillRect(bauAQ.x + 6, bauAQ.y + 8, 34, 28, COL_BAU_BG);
  drawBauHalfCircle(bauAQ.x + 22, bauAQ.y + 32, 14, aqCol);
  tft.fillRect(253, 17, 19, 10, COL_BAU_BG);
  drawBauHalfCircle(262, 26, 8, aqCol);

  uint16_t ignored;
  const char *aqLabel = airQualityLabel(rd.gasKOhm, ignored);
  clearBauValue(bauAQ);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(aqLabel, bauAQ.x + 12, bauAQ.y + 62);
  snprintf(buf, sizeof(buf), "%.0f kOhm", rd.gasKOhm);
  tft.drawString(buf, bauAQ.x + 12, bauAQ.y + 86);
}

// ---------------------------------------------------------------------------
// Design switching and measurement
// ---------------------------------------------------------------------------

void drawStaticLayout() {
  switch (currentDesign) {
    case DESIGN_TILES: drawStaticTiles(); break;
    case DESIGN_TERM: drawStaticTerm(); break;
    case DESIGN_BAUHAUS: drawStaticBauhaus(); break;
    default: drawStaticLcars(); break;
  }
}

void drawReadings(const Readings &rd) {
  switch (currentDesign) {
    case DESIGN_TILES: drawReadingsTiles(rd); break;
    case DESIGN_TERM: drawReadingsTerm(rd); break;
    case DESIGN_BAUHAUS: drawReadingsBauhaus(rd); break;
    default: drawReadingsLcars(rd); break;
  }
}

// step = +1 (next design) or -1 (previous design).
void switchDesign(int8_t step) {
  currentDesign = (currentDesign + DESIGN_COUNT + step) % DESIGN_COUNT;
  prefs.putUChar("design", currentDesign);
  settings.design = currentDesign;
  if (saveSettingsToSd()) {
    Serial.println("SD: settings saved");
  }
  drawStaticLayout();
  // Redraw the last readings immediately instead of waiting for the interval.
  if (hasReading) {
    drawReadings(lastReading);
  }
  Serial.printf("Design switched: %s\n", DESIGN_NAMES[currentDesign]);
}

#ifdef SIMULATE_SENSOR
// Produces slowly drifting fake values so the layout can be checked on the
// real display without a BME680 attached. The air quality deliberately
// cycles through all three status levels.
void simulateReading(Readings &rd) {
  float t = millis() / 1000.0f;
  rd.tempC = 21.5f + 3.0f * sinf(t / 15.0f);
  rd.humidity = 45.0f + 15.0f * sinf(t / 22.0f + 1.0f);
  rd.pressureHpa = 1013.0f + 4.0f * sinf(t / 40.0f + 2.0f);
  rd.gasKOhm = 45.0f + 35.0f * sinf(t / 18.0f + 0.5f);
}
#endif

void measureAndShow() {
  Readings rd;
#ifdef SIMULATE_SENSOR
  simulateReading(rd);
#else
  if (!bme.performReading()) {
    Serial.println("BME680: reading failed");
    return;
  }

  rd.tempC = bme.temperature;
  rd.humidity = bme.humidity;
  rd.pressureHpa = bme.pressure / 100.0f;
  rd.gasKOhm = bme.gas_resistance / 1000.0f;
#endif

  lastReading = rd;
  hasReading = true;
  drawReadings(rd);

  Serial.printf("T=%.1fC  RH=%.1f%%  P=%.1fhPa  Gas=%.0fkOhm\n",
                rd.tempC, rd.humidity, rd.pressureHpa, rd.gasKOhm);
}

void setupPalette() {
  COL_BG = tft.color565(0x00, 0x00, 0x00);
  COL_ORANGE = tft.color565(0xFF, 0x9F, 0x41);
  COL_LILAC = tft.color565(0xCC, 0x99, 0xCC);
  COL_PEACH = tft.color565(0xFF, 0x7F, 0x66);
  COL_PALEBLUE = tft.color565(0x99, 0xCC, 0xFF);
  COL_TAN = tft.color565(0xFF, 0xCC, 0x66);
  COL_BLACK_TEXT = tft.color565(0x00, 0x00, 0x00);
  COL_MUTED = tft.color565(0x8C, 0x8C, 0xAA);
  COL_READOUT = tft.color565(0xFF, 0xE0, 0xB3);
  COL_COOL = tft.color565(0x99, 0xCC, 0xFF);
  COL_GOOD = tft.color565(0x66, 0xCC, 0x99);
  COL_WARN = tft.color565(0xFF, 0x99, 0x66);
  COL_AQ_GOOD = tft.color565(0x66, 0xCC, 0x99);
  COL_AQ_MID = tft.color565(0xFF, 0xCC, 0x66);
  COL_AQ_BAD = tft.color565(0xCC, 0x66, 0x66);
  COL_NIGHT = tft.color565(0x10, 0x14, 0x1A);
  COL_TILE = tft.color565(0x1C, 0x22, 0x2B);
  COL_TILE_TEXT = tft.color565(0xE8, 0xED, 0xF4);
  COL_TERM_BRIGHT = tft.color565(0x50, 0xFF, 0x80);
  COL_TERM_DIM = tft.color565(0x20, 0xC0, 0x40);
  COL_BAU_BG = tft.color565(0xF2, 0xED, 0xE3);
  COL_BAU_RED = tft.color565(0xD9, 0x30, 0x1D);
  COL_BAU_BLUE = tft.color565(0x1E, 0x56, 0xA0);
  COL_BAU_YELLOW = tft.color565(0xF2, 0xB6, 0x0A);
}

void setup() {
  Serial.begin(115200);

  // Turn on the backlight.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1); // 0-3, adjust to the desired orientation
  setupPalette();

  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1); // must match the display rotation

  // Load the last selected design from NVS.
  prefs.begin("cyd", false);
  currentDesign = prefs.getUChar("design", DESIGN_LCARS);
  if (currentDesign >= DESIGN_COUNT) {
    currentDesign = DESIGN_LCARS;
  }

  // Settings from the SD card take precedence over NVS. If a card is
  // present but has no settings file yet, create one as an editable
  // template reflecting the current state.
  settings.design = currentDesign;
  if (loadSettingsFromSd()) {
    currentDesign = settings.design;
  } else if (sdAvailable) {
    saveSettingsToSd();
  }

  drawStaticLayout();

#ifdef SIMULATE_SENSOR
  Serial.printf("CYD room climate (SIMULATION, no BME680 needed) running. Design: %s\n",
                DESIGN_NAMES[currentDesign]);
#else
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x76) && !bme.begin(0x77)) {
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&FreeSansBold12pt7b);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("BME680 NICHT GEFUNDEN", 10, 220);
    Serial.println("BME680 not found - check wiring (SDA=27, SCL=22)");
    while (true) {
      delay(1000);
    }
  }

  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // heater plate: 320 C for 150 ms

  Serial.printf("CYD room climate measurement running. Design: %s\n",
                DESIGN_NAMES[currentDesign]);
#endif
}

void loop() {
  // Tapping the right half of the display switches to the next design,
  // tapping the left half to the previous one. Thanks to
  // touch.setRotation(1), p.x is the display's horizontal axis (as a raw
  // value, hence the comparison against the range midpoint, not pixels).
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    int8_t step = (p.x >= TOUCH_RAW_MID) ? +1 : -1;
    if (settings.touchSwap) {
      step = -step;
    }
    switchDesign(step);
    // Wait until the finger is lifted, otherwise designs would rattle through.
    while (touch.touched()) {
      delay(10);
    }
    delay(150); // debounce
    return;
  }

  unsigned long now = millis();
  if (!hasReading || now - lastMeasurement >= settings.intervalMs) {
    lastMeasurement = now;
    measureAndShow();
  }
}

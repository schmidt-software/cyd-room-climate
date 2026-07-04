#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
// FreeSans*-/FreeMono*-Fonts sind bereits ueber LOAD_GFXFF in TFT_eSPI.h eingebunden.
// Antonio Bold (kondensiert) fuer das LCARS-Design, generiert aus der TTF.
#include "fonts_antonio.h"
#ifndef SIMULATE_SENSOR
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#endif

// Der XPT2046-Touchcontroller haengt beim CYD an einem eigenen SPI-Bus,
// getrennt vom Display (siehe PINS.md im CYD-Repo).
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_IRQ 36

// Der XPT2046 liefert Rohwerte (ca. 200-3900). Fuer die Unterscheidung
// linke/rechte Displayhaelfte reicht die Bereichsmitte, eine echte
// Kalibrierung ist dafuer nicht noetig.
#define TOUCH_RAW_MID 2048

// Onboard-I2C-Anschluss des CYD (JST-Stecker CN1: GND, IO22, IO27, 3V3),
// fuer den BME680-Sensor. Verkabelung siehe README, "BME680 anschliessen".
#define I2C_SDA 27
#define I2C_SCL 22

// Grobe Einordnung der Luftqualitaet anhand des Gaswiderstands (kOhm).
// Kein kalibrierter IAQ-Index (dafuer waere die Bosch-BSEC-Bibliothek noetig),
// aber fuer eine grobe "gut/mittel/schlecht"-Anzeige ausreichend.
#define GAS_GOOD_KOHM 50.0f
#define GAS_MODERATE_KOHM 20.0f

#define MEASURE_INTERVAL_MS 2000

// Drei umschaltbare Designs. Antippen des Touchscreens wechselt zum
// naechsten; die Auswahl wird im NVS gespeichert und ueberlebt Neustarts.
enum : uint8_t { DESIGN_LCARS = 0, DESIGN_TILES, DESIGN_TERM, DESIGN_BAUHAUS, DESIGN_COUNT };
const char *DESIGN_NAMES[DESIGN_COUNT] = {"LCARS", "KACHELN", "TERMINAL", "BAUHAUS"};

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

// Farbpalette; Werte werden einmalig in setup() via color565 befuellt.
// LCARS (Star-Trek-Computerinterface): schwarzer Hintergrund, warme
// Orange-/Lilatoene. Dazu Farben fuer das Kachel- und das Terminal-Design.
uint16_t COL_BG, COL_ORANGE, COL_LILAC, COL_PEACH, COL_PALEBLUE, COL_TAN,
    COL_BLACK_TEXT, COL_MUTED, COL_READOUT, COL_COOL, COL_GOOD, COL_WARN,
    COL_AQ_GOOD, COL_AQ_MID, COL_AQ_BAD,
    COL_NIGHT, COL_TILE, COL_TILE_TEXT,
    COL_TERM_BRIGHT, COL_TERM_DIM,
    COL_BAU_BG, COL_BAU_RED, COL_BAU_BLUE, COL_BAU_YELLOW;

// ---------------------------------------------------------------------------
// Gemeinsame Helfer (Statusfarben, Luftqualitaet, Gradzeichen)
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
  if (gasKOhm >= GAS_GOOD_KOHM) {
    color = COL_AQ_GOOD;
    return "GUT";
  }
  if (gasKOhm >= GAS_MODERATE_KOHM) {
    color = COL_AQ_MID;
    return "MITTEL";
  }
  color = COL_AQ_BAD;
  return "SCHLECHT";
}

// "C" alleine waere die falsche Einheit - "C" ist nicht "Grad Celsius".
// Die GFX-Fonts kennen aber kein Gradzeichen (nur ASCII 0x20-0x7E), daher
// wird es als kleiner hochgestellter Kreis vor dem "C" gezeichnet.
// anchorX/anchorY: MR-Anker (rechtsbuendig, vertikal mittig) fuer das "C".
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

// Vier LCARS-Readout-Zeilen im Inhaltsbereich rechts der Seitenleiste.
Row rowTemp = {64, 68, 252, 36};
Row rowHum = {64, 112, 252, 36};
Row rowPress = {64, 156, 252, 36};
Row rowAQ = {64, 200, 252, 36};

// Sternenflotten-Delta als Silhouette, dem Original nachempfunden.
// Die Kontur wird zeilenweise aus zwei Kurven aufgebaut: aussen die
// geschwungene Flanke von der schmalen Spitze zu den Fluegelspitzen
// (x waechst mit Exponent 1.35, dadurch oben schlank und unten
// ausschwingend), innen ab ~60% Hoehe die konkave Kerbe (Exponent 0.45,
// dadurch breit und flach ausgerundet). Zeilenweises Fuellen haelt die
// duenn auslaufenden Fluegel lueckenlos geschlossen.
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

// Zeichnet ein Rechteck, das nur links abgerundet ist (LCARS-Endstueck),
// indem zunaechst eine volle Kapselform gezeichnet und die rechte Haelfte
// anschliessend quadratisch uebermalt wird.
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
  // Transparent statt deckend: ein zu breites Label liefe sonst als
  // farbiges Rechteck ueber den dunklen Messwert-Bereich rechts daneben.
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

  // Elbow: die klassische LCARS-Eckverbindung oben links.
  tft.fillRoundRect(4, 4, ELBOW_SIZE, ELBOW_SIZE, 18, COL_ORANGE);

  // Sternenflotten-Delta als schwarze Silhouette im Elbow.
  drawStarfleetDelta(32, 9, 36, 44, COL_BLACK_TEXT);

  // Kopfleiste rechts vom Elbow.
  tft.fillRoundRect(64, 4, 252, 26, 13, COL_ORANGE);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&Antonio_Bold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_ORANGE);
  tft.drawString("RAUMKLIMA", 76, 17);
  tft.setTextDatum(MR_DATUM);
  tft.setFreeFont(&Antonio_Bold9pt7b);
  tft.drawString("NCC-2432", 308, 17);

  // Seitenleiste unterhalb des Elbows.
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
// Design 2: Kacheln - dunkles 2x2-Grid mit grossen Werten
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

// Loescht nur die Wertezone in der Kachelmitte; Label (oben) und
// Einheit (unten rechts) bleiben stehen.
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
  // Gaswiderstand ist dynamisch, daher eigene Loeschzone unten rechts.
  tft.fillRect(tileAQ.x + 6, tileAQ.y + tileAQ.h - 26, tileAQ.w - 12, 20, COL_TILE);
  snprintf(buf, sizeof(buf), "%.0f kOhm", rd.gasKOhm);
  drawTileUnit(tileAQ, buf);
}

// ---------------------------------------------------------------------------
// Design 3: Terminal - monochrom-gruene Retro-Konsole
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

  // Invertierte Kopfzeile wie bei einem alten Datenterminal.
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

  // Temperatur: Wert, dann Gradkreis + "C" von Hand (Fonts sind ASCII-only).
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
// Design 4: Bauhaus - Grundformen in Primaerfarben auf Papierweiss,
// getrennt durch kraeftige schwarze Rasterlinien. Formen-Farb-Zuordnung
// nach Kandinsky: Kreis=Blau, Quadrat=Rot, Dreieck=Gelb.
// ---------------------------------------------------------------------------

struct BauCell {
  int16_t x, y, w, h;
};

// 2x2-Raster; die Luecken dazwischen fuellen die 4px-Rasterlinien.
BauCell bauTemp = {0, 40, 158, 96};
BauCell bauHum = {162, 40, 158, 96};
BauCell bauPress = {0, 140, 158, 100};
BauCell bauAQ = {162, 140, 158, 100};

// Statusfarbe der Luftguete aus der festen Primaerpalette:
// Blau=gut, Gelb=mittel, Rot=schlecht.
uint16_t bauhausAqColor(float gasKOhm) {
  if (gasKOhm >= GAS_GOOD_KOHM) return COL_BAU_BLUE;
  if (gasKOhm >= GAS_MODERATE_KOHM) return COL_BAU_YELLOW;
  return COL_BAU_RED;
}

// Halbkreis mit flacher Seite unten - das vierte Element im Formenkanon,
// eigenstaendig neben Kreis, Quadrat und Dreieck. Gezeichnet als Vollkreis,
// dessen untere Haelfte mit dem Hintergrund uebermalt wird (wie drawLeftCap).
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

// Loescht die Wertezone unterhalb von Form und Label.
void clearBauValue(const BauCell &c) {
  tft.fillRect(c.x + 4, c.y + 34, c.w - 8, 58, COL_BAU_BG);
}

void drawBauValue(const BauCell &c, const char *value, const GFXfont *font) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(font);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(value, c.x + 12, c.y + 62);
}

// Einheit klein hinter dem grossen Messwert; die y-Position ist so
// gewaehlt, dass die Grundlinien von 24pt-Wert und 9pt-Einheit buendig
// liegen. xStart ist der Abstand vom linken Zellenrand.
void drawBauUnit(const BauCell &c, int16_t xStart, const char *unit) {
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString(unit, c.x + xStart, c.y + 72);
}

void drawStaticBauhaus() {
  tft.fillScreen(COL_BAU_BG);

  // Kopfzeile: Titel, die drei Grundformen, SIM/LIVE-Kennung.
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(COL_BLACK_TEXT, COL_BAU_BG);
  tft.drawString("RAUMKLIMA", 10, 18);

  // Kreis, Quadrat, Dreieck; der vierte Platz rechts daneben gehoert dem
  // Status-Halbkreis der Luftguete, der dynamisch in drawReadingsBauhaus
  // gezeichnet wird.
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

  // Rasterlinien.
  tft.fillRect(0, 36, 320, 4, COL_BLACK_TEXT);
  tft.fillRect(0, 136, 320, 4, COL_BLACK_TEXT);
  tft.fillRect(158, 40, 4, 200, COL_BLACK_TEXT);

  // Formen und Labels der Zellen; der Status-Halbkreis der Luftguete-Zelle
  // wird dynamisch in drawReadingsBauhaus gezeichnet.
  // Kurzlabels, damit sie in 12pt neben die Formen passen.
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
  vw = tft.textWidth(buf); // Font ist noch der 24pt-Wertefont
  // Gradkreis von Hand vor dem "C" (Fonts sind ASCII-only).
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

  // Status-Halbkreis der Luftguete wechselt die Farbe mit dem Messwert:
  // einmal gross in der Zelle, einmal klein in der Kopfzeile als viertes
  // Element neben den drei Grundformen. Der kleine nutzt r=8, damit die
  // Uebermal-Zone des Helfers oberhalb der Rasterlinie (y=36) bleibt.
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
// Design-Umschaltung und Messung
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

// step = +1 (naechstes Design) oder -1 (vorheriges Design).
void switchDesign(int8_t step) {
  currentDesign = (currentDesign + DESIGN_COUNT + step) % DESIGN_COUNT;
  prefs.putUChar("design", currentDesign);
  drawStaticLayout();
  // Letzte Messwerte sofort wieder anzeigen statt auf das Intervall zu warten.
  if (hasReading) {
    drawReadings(lastReading);
  }
  Serial.printf("Design gewechselt: %s\n", DESIGN_NAMES[currentDesign]);
}

#ifdef SIMULATE_SENSOR
// Erzeugt langsam schwankende Fantasiewerte, damit sich das Layout ohne
// angeschlossenen BME680 am echten Display pruefen laesst. Die Luftqualitaet
// durchlaeuft dabei bewusst alle drei Ampelstufen.
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
    Serial.println("BME680: Messung fehlgeschlagen");
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

  // Backlight einschalten
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1); // 0-3, je nach gewuenschter Ausrichtung anpassen
  setupPalette();

  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1); // muss zur Display-Rotation passen

  // Zuletzt gewaehltes Design aus dem NVS laden.
  prefs.begin("cyd", false);
  currentDesign = prefs.getUChar("design", DESIGN_LCARS);
  if (currentDesign >= DESIGN_COUNT) {
    currentDesign = DESIGN_LCARS;
  }

  drawStaticLayout();

#ifdef SIMULATE_SENSOR
  Serial.printf("CYD Raumklima (SIMULATION, kein BME680 noetig) laeuft. Design: %s\n",
                DESIGN_NAMES[currentDesign]);
#else
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x76) && !bme.begin(0x77)) {
    tft.setTextDatum(TL_DATUM);
    tft.setFreeFont(&FreeSansBold12pt7b);
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("BME680 NICHT GEFUNDEN", 10, 220);
    Serial.println("BME680 nicht gefunden - Verkabelung pruefen (SDA=27, SCL=22)");
    while (true) {
      delay(1000);
    }
  }

  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // Heizplatte: 320 C fuer 150 ms

  Serial.printf("CYD Raumklima-Messung laeuft. Design: %s\n",
                DESIGN_NAMES[currentDesign]);
#endif
}

void loop() {
  // Tippen auf die rechte Displayhaelfte wechselt zum naechsten Design,
  // auf die linke Haelfte zum vorherigen. p.x ist dank touch.setRotation(1)
  // die Querachse des Displays (als Rohwert, daher Vergleich mit der
  // Bereichsmitte statt mit Pixeln).
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    switchDesign(p.x >= TOUCH_RAW_MID ? +1 : -1);
    // Warten bis der Finger wieder weg ist, sonst rattern die Designs durch.
    while (touch.touched()) {
      delay(10);
    }
    delay(150); // Entprellen
    return;
  }

  unsigned long now = millis();
  if (!hasReading || now - lastMeasurement >= MEASURE_INTERVAL_MS) {
    lastMeasurement = now;
    measureAndShow();
  }
}

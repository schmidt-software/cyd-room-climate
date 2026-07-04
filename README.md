# CYD Raumklima

PlatformIO-Projekt fuer den ESP32 Cheap Yellow Display (ESP32-2432S028R),
**USB-C-Variante** mit resistivem Touchscreen.
Misst Temperatur, Luftfeuchte, Luftdruck und bewertet die Luftqualitaet ueber
einen BME680-Sensor. Tippen auf die rechte Displayhaelfte wechselt zum
naechsten, auf die linke zum vorherigen der vier Designs (LCARS, Kacheln,
Terminal, Bauhaus); die Auswahl bleibt auch nach einem Neustart erhalten.

## Voraussetzungen

- VS Code mit der Erweiterung **PlatformIO IDE**
- CYD-Board per USB-C mit dem Rechner verbunden
- Ggf. CH340/CP2102 USB-Treiber installiert, falls das Board nicht erkannt wird
- **BME680-Breakout** (z. B. Adafruit, DFRobot oder generisches "GY-BME680"), angeschlossen an den
  onboard JST-Stecker **CN1** des CYD (Details siehe Abschnitt "BME680 anschliessen")

## Simulationsmodus (ohne Sensor)

Solange der BME680 noch nicht angeschlossen ist, erzeugt das Programm dank des Build-Flags
`-DSIMULATE_SENSOR=1` in `platformio.ini` Fantasiewerte, die sich langsam veraendern
(die Luftqualitaet durchlaeuft dabei absichtlich Gut/Mittel/Schlecht). So laesst sich das
Dashboard-Layout schon jetzt auf dem echten Display pruefen. Oben rechts erscheint dazu der
Hinweis "SIMULATION". **Sobald der Sensor angeschlossen ist**, die Zeile `-DSIMULATE_SENSOR=1`
in `platformio.ini` entfernen und neu flashen, um echte Messwerte zu erhalten.

## Verwendung

1. Diesen Ordner in VS Code oeffnen (PlatformIO erkennt ihn automatisch als Projekt).
2. BME680 gemaess obiger Pinbelegung an das CYD anschliessen.
3. Unten in der PlatformIO-Statusleiste auf **Upload** klicken (oder `pio run --target upload` im Terminal).
4. PlatformIO laedt automatisch Toolchain und Bibliotheken (TFT_eSPI, XPT2046_Touchscreen, Adafruit BME680) herunter, kompiliert und flasht den ESP32.
5. Nach dem Flashen zeigt das Display Temperatur, Luftfeuchte, Luftdruck und eine Luftqualitaets-Einschaetzung (Gut/Mittel/Schlecht), alle 2 Sekunden aktualisiert. Tippen rechts wechselt zum naechsten, links zum vorherigen Design. Werte werden zusaetzlich ueber den seriellen Monitor ausgegeben (115200 Baud).

## Designs

Tippen auf die rechte Displayhaelfte wechselt zyklisch zum naechsten Design,
Tippen auf die linke Haelfte zum vorherigen. Die Auswahl wird im NVS
(Preferences) gespeichert und beim naechsten Start wiederhergestellt.

1. **LCARS** — Star-Trek-Computerinterface: Elbow, Seitenleiste und
   Readout-Zeilen in warmen Orange-/Lilatoenen auf Schwarz. Schrift:
   Antonio Bold (kondensiert, SIL-OFL-lizenziert) als freier Ersatz fuer
   das LCARS-Original "Swiss 911 Ultra Compressed", eingebettet als
   generierte GFX-Fonts in `src/fonts_antonio.h`.
2. **Kacheln** — modernes Dashboard: 2x2-Grid dunkler Kacheln mit grossen,
   nach Status eingefaerbten Werten.
3. **Terminal** — Retro-Konsole: monochrom-gruene Monospace-Anzeige mit
   invertierter Kopfzeile.
4. **Bauhaus** — Grundformen in Primaerfarben auf Papierweiss: Kreis (Blau),
   Quadrat (Rot) und Dreieck (Gelb) nach Kandinsky, 2x2-Raster mit kraeftigen
   schwarzen Linien; die Luftguete zeigt ein eigenstaendiges viertes Symbol,
   einen Halbkreis, der die Farbe mit dem Messwert wechselt
   (Blau=gut, Gelb=mittel, Rot=schlecht). Er erscheint gross in der
   Luftguete-Zelle und klein in der Kopfzeile neben den drei Grundformen.

## Hardware-Details

- **Display**: ST7789-Panel-Variante des CYD (`ST7789_DRIVER` mit
  `TFT_RGB_ORDER=TFT_RGB` und `TFT_INVERSION_OFF` in `platformio.ini`).
  Andere CYD-Chargen haben ein ILI9341-Panel und brauchen stattdessen
  `ILI9341_DRIVER` bzw. `ILI9341_2_DRIVER`.
- **Touch**: XPT2046 (resistiv) am eigenen SPI-Bus — CLK=25, CS=33, MOSI=32, MISO=39, IRQ=36.
- **Sensor**: Bosch BME680 (I2C) — Temperatur, rel. Luftfeuchte, Luftdruck, Gaswiderstand.
  Adresse wird automatisch erkannt (0x76 oder 0x77).

## Breakout-Anschluesse des CYD

Auf der Rueckseite des Boards sitzen vier JST-Stecker (1,25 mm Rastermass):

| Stecker | Pins | Zweck |
|---------|------|-------|
| P1 | VIN, TX, RX, GND | Seriell / Stromversorgung |
| **CN1** | GND, GPIO 22, GPIO 27, 3.3V | **I2C-Anschluss (hier: BME680)** |
| P3 | GND, GPIO 35, GPIO 22, GPIO 21 | Zusatz-IO (GPIO 35 nur Eingang) |
| P4 / SPEAK | Lautsprecher-Ausgang (GPIO 26 ueber Verstaerker) | Audio |

Weitere fest belegte Pins: microSD-Slot (GPIO 5/18/19/23), RGB-LED
(GPIO 4/16/17), Helligkeitssensor/LDR (GPIO 34), Display und Touch (siehe
[offizielle PINS.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md)).
Frei nutzbar sind damit im Wesentlichen nur die Pins der Stecker CN1 und P3.

## BME680 anschliessen

Der Sensor kommt an den Stecker **CN1** (die Pin-Reihenfolge vorsichtshalber
mit dem Platinenaufdruck abgleichen):

| CN1-Pin | Signal | BME680-Breakout |
|---------|--------|-----------------|
| 1 | GND | GND |
| 2 | GPIO 22 (SCL) | SCL (bei Adafruit: SCK) |
| 3 | GPIO 27 (SDA) | SDA (bei Adafruit: SDI) |
| 4 | 3.3V | VCC / VIN |

- **CS und SDO** am Breakout fuer I2C-Betrieb frei lassen. SDO bestimmt die
  I2C-Adresse (offen/GND = 0x76, an VCC = 0x77) - die Firmware probiert beim
  Start automatisch beide.
- Der BME680 laeuft mit 3,3 V direkt vom CN1-Pin 4; die gaengigen Breakouts
  (Adafruit, GY-BME680) haben zusaetzlich einen eigenen Spannungsregler und
  vertragen daher auch 5 V - noetig ist das hier nicht.
- Nach dem Anschliessen die Zeile `-DSIMULATE_SENSOR=1` in `platformio.ini`
  entfernen und neu flashen: Die Anzeige wechselt von "SIM" auf "LIVE" und
  zeigt echte Messwerte.

## Hinweis zur Luftqualitaet

Die Anzeige "Gut/Mittel/Schlecht" basiert auf einer einfachen Schwellwert-Einteilung
des rohen Gaswiderstands (kOhm) und ist **kein kalibrierter IAQ-Index**. Fuer eine
praezisere, kalibrierte Bewertung (inkl. Einbrennzeit und Baseline-Tracking) kann
spaeter die Bosch-BSEC-Bibliothek ergaenzt werden.

## Falls es nicht passt

- **Farben invertiert/falsch**: je nach Panel-Variante in `platformio.ini` den Treiber wechseln (`ST7789_DRIVER`, `ILI9341_DRIVER` oder `ILI9341_2_DRIVER`); bei vertauschtem Rot/Blau zusaetzlich `TFT_RGB_ORDER` zwischen `TFT_RGB` und `TFT_BGR` umstellen.
- **Touch spiegelverkehrt oder versetzt**: `touch.setRotation(...)` in `src/main.cpp` anpassen. Sind dadurch links/rechts beim Designwechsel vertauscht, alternativ einfach das Vorzeichen beim `switchDesign(...)`-Aufruf in `loop()` tauschen.
- **Anderes Pinout**: Pin-Zuordnung anhand der [offiziellen PINS.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md) pruefen.
- **Board wird nicht erkannt**: passenden USB-Treiber (CH340 oder CP2102, je nach verbautem Chip) installieren.

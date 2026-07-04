# CYD Room Climate

PlatformIO project for the ESP32 Cheap Yellow Display (ESP32-2432S028R)
with a resistive touchscreen. Measures temperature, humidity and barometric
pressure and rates air quality using a BME680 sensor. Tapping the right half
of the display switches to the next of four designs (LCARS, Tiles, Terminal,
Bauhaus), tapping the left half to the previous one; the selection persists
across reboots.

Note: the on-screen UI labels are in German (RAUMKLIMA = room climate,
FEUCHT(E) = humidity, DRUCK = pressure, LUFTQ/LUFTGUETE = air quality,
GUT/MITTEL/SCHLECHT = good/medium/poor).

## Requirements

- VS Code with the **PlatformIO IDE** extension
- CYD board connected via USB
- CH340/CP2102 USB driver installed if the board is not detected
- **BME680 breakout** (e.g. Adafruit, DFRobot or a generic "GY-BME680"),
  connected to the onboard JST connector **CN1** (see "Connecting the BME680")

## Simulation mode (without a sensor)

As long as no BME680 is attached, the build flag `-DSIMULATE_SENSOR=1` in
`platformio.ini` makes the program generate slowly drifting fake values (the
air quality deliberately cycles through good/medium/poor). This lets you
check the dashboard layout on the real display right away; a "SIM" badge is
shown on screen. **Once the sensor is connected**, remove the
`-DSIMULATE_SENSOR=1` line from `platformio.ini` and reflash to get real
readings.

## Usage

1. Open this folder in VS Code (PlatformIO picks it up as a project automatically).
2. Connect the BME680 as described below.
3. Click **Upload** in the PlatformIO status bar (or run `pio run --target upload` in a terminal).
4. PlatformIO automatically downloads the toolchain and libraries (TFT_eSPI, XPT2046_Touchscreen, Adafruit BME680), compiles and flashes the ESP32.
5. After flashing, the display shows temperature, humidity, pressure and an air-quality rating (good/medium/poor), refreshed every 2 seconds. Tap right for the next design, left for the previous one. Readings are also printed to the serial monitor (115200 baud).

## Designs

Tapping the right half of the display cycles to the next design, tapping the
left half to the previous one. The selection is stored in NVS (Preferences)
and restored on the next boot.

1. **LCARS** — Star Trek computer interface: elbow, sidebar and readout
   rows in warm orange/lilac tones on black. Typeface: Antonio Bold
   (condensed, SIL-OFL licensed) as a free substitute for the original
   LCARS font "Swiss 911 Ultra Compressed", embedded as generated GFX
   fonts in `src/fonts_antonio.h`. The elbow carries a Starfleet delta
   silhouette.
2. **Tiles** — modern dashboard: a 2x2 grid of dark tiles with large,
   status-colored values.
3. **Terminal** — retro console: monochrome green monospace output with an
   inverted header line.
4. **Bauhaus** — basic shapes in primary colors on paper white: circle
   (blue), square (red) and triangle (yellow) after Kandinsky, in a 2x2
   grid with bold black lines; the air quality uses a distinct fourth
   symbol, a half circle, which changes color with the reading
   (blue=good, yellow=medium, red=poor). It appears large in the
   air-quality cell and small in the header next to the three basic shapes.

## Hardware details

- **Display**: ST7789 panel variant of the CYD (`ST7789_DRIVER` with
  `TFT_RGB_ORDER=TFT_BGR` and `TFT_INVERSION_OFF` in `platformio.ini`).
  Other CYD batches ship with an ILI9341 panel and need `ILI9341_DRIVER`
  or `ILI9341_2_DRIVER` instead.
- **Touch**: XPT2046 (resistive) on its own SPI bus — CLK=25, CS=33, MOSI=32, MISO=39, IRQ=36.
- **Sensor**: Bosch BME680 (I2C) — temperature, relative humidity, pressure, gas resistance.
  The address is detected automatically (0x76 or 0x77).

## CYD breakout connectors

The back of the board has four JST connectors (1.25 mm pitch):

| Connector | Pins | Purpose |
|-----------|------|---------|
| P1 | VIN, TX, RX, GND | Serial / power |
| **CN1** | GND, GPIO 22, GPIO 27, 3.3V | **I2C connector (used here: BME680)** |
| P3 | GND, GPIO 35, GPIO 22, GPIO 21 | Extra IO (GPIO 35 is input-only) |
| P4 / SPEAK | Speaker output (GPIO 26 via amplifier) | Audio |

Other permanently assigned pins: microSD slot (GPIO 5/18/19/23), RGB LED
(GPIO 4/16/17), light sensor/LDR (GPIO 34), display and touch (see the
[official PINS.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md)).
In practice only the pins on CN1 and P3 are freely usable.

## Connecting the BME680

The sensor connects to the **CN1** connector (double-check the pin order
against the silkscreen on the board):

| CN1 pin | Signal | BME680 breakout |
|---------|--------|-----------------|
| 1 | GND | GND |
| 2 | GPIO 22 (SCL) | SCL (Adafruit: SCK) |
| 3 | GPIO 27 (SDA) | SDA (Adafruit: SDI) |
| 4 | 3.3V | VCC / VIN |

- Leave **CS and SDO** on the breakout unconnected for I2C operation. SDO
  selects the I2C address (open/GND = 0x76, tied to VCC = 0x77) - the
  firmware automatically tries both at startup.
- The BME680 runs directly off the 3.3 V from CN1 pin 4; the common
  breakouts (Adafruit, GY-BME680) additionally have their own voltage
  regulator and therefore tolerate 5 V as well - not needed here.
- After wiring, remove the `-DSIMULATE_SENSOR=1` line from
  `platformio.ini` and reflash: the badge switches from "SIM" to "LIVE"
  and real readings are shown.

## Settings on the microSD card

If a microSD card (FAT32) is inserted, the firmware stores its settings in
`/settings.txt` on the card:

- **At boot** the file is read and overrides the defaults (and the design
  stored in NVS). If a card is present but has no settings file yet, a
  self-documenting template is created automatically.
- **On every design change** the file is rewritten, so the card always
  reflects the current state.
- **Without a card** everything works as before; the selected design then
  persists in NVS only.

Available keys (unknown keys and out-of-range values are ignored):

| Key | Values | Meaning |
|-----|--------|---------|
| `design` | `lcars`, `tiles`, `terminal`, `bauhaus` | Active design |
| `interval_ms` | 500-3600000 | Measurement interval in milliseconds |
| `touch_swap` | 0 or 1 | Swaps the left/right tap direction |
| `gas_good_kohm` | > 0 | Air quality "good" threshold (gas resistance) |
| `gas_moderate_kohm` | > 0 | Air quality "medium" threshold |

To change settings from a PC, edit `/settings.txt` on the card and reboot
the device (settings are only read at startup).

Technical note: the SD slot (CS=5, SCK=18, MISO=19, MOSI=23) shares SPI
usage with the touch controller; the firmware re-points the bus to the SD
pins for each card access and back to the touch pins afterwards.

## Note on air quality

The good/medium/poor rating is a simple threshold classification of the raw
gas resistance (kOhm) and is **not a calibrated IAQ index**. For a more
precise, calibrated rating (including burn-in and baseline tracking) the
Bosch BSEC library can be added later.

## Troubleshooting

- **Colors inverted/wrong**: switch the driver in `platformio.ini` depending on the panel variant (`ST7789_DRIVER`, `ILI9341_DRIVER` or `ILI9341_2_DRIVER`); if red and blue are swapped, also toggle `TFT_RGB_ORDER` between `TFT_RGB` and `TFT_BGR`.
- **Touch mirrored or offset**: adjust `touch.setRotation(...)` in `src/main.cpp`. If that swaps left/right for design switching, simply flip the sign in the `switchDesign(...)` call in `loop()`.
- **Different pinout**: check the pin mapping against the [official PINS.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md).
- **Board not detected**: install the matching USB driver (CH340 or CP2102, depending on the chip on the board).
- **Upload aborts** ("Serial data stream stopped"): the CH340 on the CYD is unreliable at 921600 baud; this project therefore uses `upload_speed = 460800`.

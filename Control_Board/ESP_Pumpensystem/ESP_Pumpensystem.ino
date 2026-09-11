/*
 * ESP32-C3 Super Mini - Standalone Mikropumpen-Regler MIT Display
 * ------------------------------------------------------------------
 * Vereint in einem Sketch:
 *  - PID-Pumpenregelung für 4 Pumpen (Logik übernommen aus Multiboard_Driver.ino),
 *    Highdriver4-Pumpentreiber an I2C-Adresse 0x79, bis 250 Vpp
 *  - Sensirion SLF3S-0600F Flusssensoren hinter einem I2C-Mux (TCA9548A/PCA9548A-
 *    Style, Adresse 0x74)
 *  - 240x280 ST7789 SPI-Display, 1.69", LANDSCAPE (width=280 / height=240),
 *    zeigt pro Pumpe: Sollfluss, Istfluss, Steuerspannung
 *
 * Required libraries (install via Arduino Library Manager):
 *   - Adafruit GFX Library
 *   - Adafruit ST7789 Library
 *   - PID (Autor: Brett Beauregard) - https://github.com/br3ttb/Arduino-PID-Library
 * (Wire und SPI sind im ESP32-Arduino-Core enthalten)
 *
 * Wiring used by this sketch:
 *   I2C:  SCL -> GPIO0   SDA -> GPIO1
 *   SPI:  SCK -> GPIO4   MOSI -> GPIO6   MISO -> GPIO7   CS -> GPIO10
 *   TFT:  DC  -> GPIO2   RST  -> GPIO3   (pick free pins, adjust if wired differently)
 *
 * Flicker fix: static layout (Titel, Zellrahmen, "P{n}"-Label) wird EINMAL in
 * setup() gezeichnet. Die loop() malt nur die kleinen Zahlenfelder neu, und
 * auch nur dann, wenn sich der Wert seit dem letzten Frame tatsächlich geändert hat.
 *
 * Rounded-corner safe area:
 *   Panel ist 1.69" Diagonale, 240x280 px -> Diagonale = sqrt(240^2+280^2) = ~368.8 px
 *   Pixelabstand = 1.69" / 368.8 px = ~0.1164 mm/px
 *   3 mm Eckenradius -> 3 / 0.1164 = ~26 px
 *   Ein einheitlicher Rand wird an allen vier Kanten frei gehalten, damit
 *   nichts unter dem abgerundeten Glas landet, auch nicht in den Ecken.
 *
 * Sensor model note:
 *   Flow scale factor is model-specific (Sensirion-Datenblatt LQ_DS_SLF3S-0600F,
 *   Tabelle "Scale factors", Abschnitt 4.5.1):
 *     SLF3S-1300F -> 500   (µl/min)^-1
 *     SLF3S-0600F -> 10    (µl/min)^-1   (in use here)
 *   Everything else (I2C address 0x08, start-continuous-measurement command
 *   0x3608, 9-byte reply frame, temperature scale factor 200) is identical
 *   across the SLF3x family.
 *
 *   KORREKTUR: Hier stand vorher fälschlich 1000 statt 10 - dadurch wurden
 *   alle Flusswerte um Faktor 100 zu klein angezeigt/geregelt. Das alte
 *   Flow_I2C.h (Multiboard_Driver.ino) hatte mit seinem festen Scale-Faktor
 *   von 10.0 tatsächlich schon den korrekten, datenblattkonformen Wert -
 *   das war umgekehrt zu meiner vorherigen Einschätzung richtig.
 */

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <PID_v1.h> // https://github.com/br3ttb/Arduino-PID-Library

// ---------------- Pin definitions ----------------
#define I2C_SCL   0
#define I2C_SDA   1

#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_MISO  7
#define TFT_CS    10
#define TFT_DC    2   // adjust if wired to a different free GPIO
#define TFT_RST   3   // adjust if wired to a different free GPIO

// ---------------- I2C addresses ----------------
#define MUX_ADDR         0x74   // 8-channel I2C mux (Flusssensoren)
#define SENSOR_ADDR      0x08   // fixed address for SLF3x sensors
#define HIGHDRIVER_ADDR  0x79   // Pumpentreiber (Highdriver4), liefert bis 250 Vpp
#define NUM_SENSORS      8      // Anzahl Mux-Kanäle (Hardware-Maximum)
#define NUM_PUMPS        4      // Anzahl tatsächlich geregelter Pumpen

// Highdriver Register-Adressen
#define I2C_DEVICEID       0x00
#define I2C_POWERMODE      0x01
#define I2C_FREQUENCY      0x02
#define I2C_SHAPE          0x03
#define I2C_BOOST          0x04
#define I2C_AUDIO          0x05
#define I2C_PVOLTAGE       0x06
#define I2C_UPDATEVOLTAGE  0x0A

// ---------------- Sensor scale factors (model-specific) ----------------
// Quelle: Sensirion-Datenblatt SLF3S-0600F, Abschnitt 4.5.1 "Scale factors".
#define FLOW_SCALE_FACTOR  10.0f    // SLF3S-0600F (use 500.0f for SLF3S-1300F)
#define TEMP_SCALE_FACTOR  200.0f   // same across the SLF3x family

// ---------------- Rounded corner safe area ----------------
// See derivation in the header comment above.
#define CORNER_SAFE_MARGIN  10  // px kept clear on every edge

// Debug-Ausgabe über Serial (I2C-Diagnose + Lesefehler). Auf 0 setzen für Produktivbetrieb.
#define DEBUG_SERIAL 0


// ============================================================
// !!! OFFEN - Pumpe <-> Flusssensor Zuordnung !!!
// ============================================================
// PUMP_SENSOR_CHANNEL[i] = Kanal (0-7) am I2C-Mux (0x74), auf dem
// der Flusssensor von Pumpe (i+1) sitzt.
//
// Platzhalter unten, 1:1 aus der alten Python-Zuordnung übernommen
// (pumpen_sensor = {"P1":2,"P2":3,"P3":1,"P4":0}). Diesen Block klären
// wir noch zusammen und passen ihn an eure tatsächliche Verkabelung an!
const uint8_t PUMP_SENSOR_CHANNEL[NUM_PUMPS] = {
  4,   // Pumpe 1 (index 0) -> Mux-Kanal 4
  5,   // Pumpe 2 (index 1) -> Mux-Kanal 5
  6,   // Pumpe 3 (index 2) -> Mux-Kanal 6
  7,   // Pumpe 4 (index 3) -> Mux-Kanal 7
  // Pumpen und Sensor Mapping angepasst 11.9.2026
};

// ------------------------------------------------------------
// Soll-Flusswerte pro Pumpe [µl/min] - fest im Code
// ------------------------------------------------------------
// Setpoint = 0  ->  Pumpe bleibt dauerhaft aus (0 V)
float PUMP_SETPOINT_ULMIN[NUM_PUMPS] = {
  100.0f,   // Pumpe 1
  100.0f,   // Pumpe 2
  100.0f,   // Pumpe 3
  100.0f,   // Pumpe 4
};

// ------------------------------------------------------------
// PID Parameter - identisch zu den bisher genutzten Werten
// aus der Python-App / Multiboard_Driver.ino
// ------------------------------------------------------------
const double PID_KP = 0.01226;
const double PID_KI = 0.101011;
const double PID_KD = 0.0;

const double PID_OUT_MIN = 1.0;      // Volt
const double PID_OUT_MAX = 250.0;    // Volt (Highdriver-Maximum)

const unsigned long PID_SAMPLE_TIME_MS = 300;  // Regelungsfrequenz (wie bisher, 300 ms)

// Tiefpass-Filter auf den Flusssensor-Messwert (wie im Python-Tool, alpha=0.4)
const float FLOW_FILTER_ALPHA = 0.4f;
const float FLOW_FILTER_START = 100.0f;


// ---------------- Objects ----------------
SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ------------------------------------------------------------
// PID-Regler-Objekte (Arduino-PID-Library) - 1 pro Pumpe
// ------------------------------------------------------------
double pidInput[NUM_PUMPS]    = {0, 0, 0, 0};
double pidOutput[NUM_PUMPS]   = {0, 0, 0, 0};
double pidSetpoint[NUM_PUMPS] = {0, 0, 0, 0};

PID pidControllers[NUM_PUMPS] = {
  PID(&pidInput[0], &pidOutput[0], &pidSetpoint[0], PID_KP, PID_KI, PID_KD, DIRECT),
  PID(&pidInput[1], &pidOutput[1], &pidSetpoint[1], PID_KP, PID_KI, PID_KD, DIRECT),
  PID(&pidInput[2], &pidOutput[2], &pidSetpoint[2], PID_KP, PID_KI, PID_KD, DIRECT),
  PID(&pidInput[3], &pidOutput[3], &pidSetpoint[3], PID_KP, PID_KI, PID_KD, DIRECT),
};

// ------------------------------------------------------------
// Highdriver Zustand
// ------------------------------------------------------------
boolean bPumpState[NUM_PUMPS]      = {false, false, false, false};
uint8_t nPumpVoltageByte[NUM_PUMPS] = {0, 0, 0, 0};
uint8_t nFrequencyByte              = 0x40;

// ------------------------------------------------------------
// Flussmessung: gefilterter Wert + Fehler-Fallback, pro Pumpe
// ------------------------------------------------------------
float lastGoodFlow[NUM_PUMPS] = {FLOW_FILTER_START, FLOW_FILTER_START, FLOW_FILTER_START, FLOW_FILTER_START};
bool  pumpSensorOk[NUM_PUMPS] = {true, true, true, true};
uint32_t fehlercount = 0;

unsigned long lastControlTime = 0;

// last-drawn text per field, so we only repaint what changed
char lastSollStr[NUM_PUMPS][12];
char lastIstStr[NUM_PUMPS][12];
char lastUStr[NUM_PUMPS][12];

// ---------------- Layout (computed in setup once rotation is set) ----------------
int gridCols = 2;
int gridRows = 2;   // 4 Pumpen statt 8 Sensor-Kanälen
int cellW, cellH;
int originX, originY;   // top-left of the usable (safe) area
int gridTop;             // y where the pump grid starts (below der Statuszeile)

int ROW_W;              // clear-rect width for each value row (set in setup(), depends on cellW)
const int ROW_H = 16;   // clear-rect height for each value row

// Statuszeile oben (zeigt laufenden Sensor-Fehlerzähler, damit man ohne
// Serial-Monitor sieht, ob es gerade I2C-Probleme gibt)
const int STATUS_BAR_H = 14;
char lastErrStr[12] = "";


// ============================================================
// Sensirion CRC8 (poly 0x31, init 0xFF)
// ============================================================
uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }
  return crc;
}

// ---------------- Mux channel select ----------------
void muxSelect(uint8_t channel) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(1 << channel);
  Wire.endTransmission();
}

// ---------------- Sensor: start continuous measurement (water) ----------------
bool sensorStartContinuous() {
  Wire.beginTransmission(SENSOR_ADDR);
  Wire.write(0x36);
  Wire.write(0x08);
  return (Wire.endTransmission() == 0);
}

// ---------------- Sensor: read one measurement frame ----------------
bool sensorRead(float &flow, float &temp) {
  uint8_t buf[9];
  if (Wire.requestFrom((int)SENSOR_ADDR, 9) != 9) return false;
  for (int i = 0; i < 9; i++) buf[i] = Wire.read();

  if (crc8(&buf[0], 2) != buf[2]) return false;
  if (crc8(&buf[3], 2) != buf[5]) return false;
  if (crc8(&buf[6], 2) != buf[8]) return false;

  int16_t rawFlow = (buf[0] << 8) | buf[1];
  int16_t rawTemp = (buf[3] << 8) | buf[4];

  flow = rawFlow / FLOW_SCALE_FACTOR;  // ul/min
  temp = rawTemp / TEMP_SCALE_FACTOR;  // deg C
  return true;
}


// ============================================================
// Highdriver (Pumpentreiber) Funktionen
// ============================================================
void Highdriver_init(void) {
  Wire.beginTransmission(HIGHDRIVER_ADDR);
  Wire.write(I2C_POWERMODE);      // Start Register 0x01
  Wire.write(0x01);               // Register 0x01 = 0x01 (enable)
  Wire.write(nFrequencyByte);     // Register 0x02 = Frequenz
  Wire.write(0x00);               // Register 0x03 = 0x00 (sine wave)
  Wire.write(0x00);               // Register 0x04 = 0x00 (800KHz)
  Wire.write(0x00);               // Register 0x05 = 0x00 (audio off)
  Wire.write(0x00);               // Register 0x06 = Amplitude1
  Wire.write(0x00);               // Register 0x07 = Amplitude2
  Wire.write(0x00);               // Register 0x08 = Amplitude3
  Wire.write(0x00);               // Register 0x09 = Amplitude4
  Wire.write(0x01);               // Register 0x0A = 0x01 (update)
  Wire.endTransmission();

  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    bPumpState[i]       = false;
    nPumpVoltageByte[i] = 0x00;
  }
}

// Setzt die Spannung EINER Pumpe (1-4) und schreibt danach den
// kompletten Spannungsblock aller 4 Pumpen an den Highdriver.
void Highdriver4_setvoltage(uint8_t _pump, uint8_t _voltage) {
  float temp = _voltage;
  temp *= 31.0f;
  temp /= 250.0f;                                                 // 250 Vpp = 0x1F (=31)
  if (_pump >= 1 && _pump <= NUM_PUMPS) nPumpVoltageByte[_pump - 1] = constrain(temp, 0, 31);

  Wire.beginTransmission(HIGHDRIVER_ADDR);
  Wire.write(I2C_PVOLTAGE);
  Wire.write((bPumpState[0] ? nPumpVoltageByte[0] : 0));
  Wire.write((bPumpState[1] ? nPumpVoltageByte[1] : 0));
  Wire.write((bPumpState[2] ? nPumpVoltageByte[2] : 0));
  Wire.write((bPumpState[3] ? nPumpVoltageByte[3] : 0));
  Wire.write(0x01);
  Wire.endTransmission();
}

// Schreibt den aktuellen Spannungsblock aller 4 Pumpen erneut
// (z. B. um eine Pumpe auf 0 V zu setzen, ohne die anderen zu verändern)
void Highdriver4_setvoltage(void) {
  Wire.beginTransmission(HIGHDRIVER_ADDR);
  Wire.write(I2C_PVOLTAGE);
  Wire.write((bPumpState[0] ? nPumpVoltageByte[0] : 0));
  Wire.write((bPumpState[1] ? nPumpVoltageByte[1] : 0));
  Wire.write((bPumpState[2] ? nPumpVoltageByte[2] : 0));
  Wire.write((bPumpState[3] ? nPumpVoltageByte[3] : 0));
  Wire.write(0x01);
  Wire.endTransmission();
}

void Highdriver_setfrequency(uint16_t _frequency) {
  if (_frequency >= 800) {
    nFrequencyByte = 0xFF;
  } else if (_frequency >= 400) {
    _frequency -= 400; _frequency *= 64; _frequency /= 400;
    nFrequencyByte = _frequency | 0xC0;
  } else if (_frequency >= 200) {
    _frequency -= 200; _frequency *= 64; _frequency /= 200;
    nFrequencyByte = _frequency | 0x80;
  } else if (_frequency >= 100) {
    _frequency -= 100; _frequency *= 64; _frequency /= 100;
    nFrequencyByte = _frequency | 0x40;
  } else if (_frequency >= 50) {
    _frequency -= 50; _frequency *= 64; _frequency /= 50;
    nFrequencyByte = _frequency | 0x00;
  } else {
    nFrequencyByte = 0x00;
  }

  Wire.beginTransmission(HIGHDRIVER_ADDR);
  Wire.write(I2C_FREQUENCY);
  Wire.write(nFrequencyByte);
  Wire.endTransmission();
}


// ============================================================
// I2C-Diagnose: prüft ohne Messung, ob Mux, Highdriver und die 4
// zugeordneten Flusssensoren überhaupt am Bus antworten.
// Läuft einmal in setup() - Ausgabe direkt auf dem TFT (kein Serial-
// Monitor nötig), zusätzlich optional über Serial (DEBUG_SERIAL).
// ============================================================
bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

// Eine Diagnosezeile zeichnen: Label in Weiß, Status in Grün/Rot.
void diagLine(int &y, const char *label, bool ok) {
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(originX, y);
  tft.print(label);
  tft.setTextColor(ok ? ST77XX_GREEN : ST77XX_RED);
  tft.print(ok ? " OK" : " FEHLT");
  y += 16;
}

// Zeigt den I2C-Check EINMAL beim Start auf dem Display an (Mux,
// Highdriver, je 1 Sensor pro Pumpe), dann geht's nach DIAG_DISPLAY_MS
// automatisch weiter zum normalen Pumpen-Grid.
#define DIAG_DISPLAY_MS 4000

void showI2CDiagnostics() {
  bool muxOk = i2cPing(MUX_ADDR);
  bool hdOk  = i2cPing(HIGHDRIVER_ADDR);

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(originX, originY);
  tft.print("I2C-Check");

  int y = originY + 28;
  diagLine(y, "Mux 0x74:", muxOk);
  diagLine(y, "Treiber 0x79:", hdOk);
  y += 6;

  for (uint8_t p = 0; p < NUM_PUMPS; p++) {
    uint8_t ch = PUMP_SENSOR_CHANNEL[p];
    muxSelect(ch);
    delay(2);
    bool sensOk = i2cPing(SENSOR_ADDR);

    char label[24];
    snprintf(label, sizeof(label), "P%d (Kanal %d):", p + 1, ch);
    diagLine(y, label, sensOk);

#if DEBUG_SERIAL
    Serial.print(label);
    Serial.println(sensOk ? " OK" : " FEHLT");
#endif
  }

#if DEBUG_SERIAL
  Serial.print("Mux 0x74: "); Serial.println(muxOk ? "OK" : "FEHLT");
  Serial.print("Treiber 0x79: "); Serial.println(hdOk ? "OK" : "FEHLT");
#endif

  delay(DIAG_DISPLAY_MS);
}

// ============================================================
// Init: Flusssensoren auf den genutzten Pumpen-Kanälen + PID-Regler
// ============================================================
void initSensorsAndPID() {
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    muxSelect(PUMP_SENSOR_CHANNEL[i]);
    delay(2);
    sensorStartContinuous();
    delay(30); // settle time after starting continuous measurement

    pidControllers[i].SetMode(AUTOMATIC);
    pidControllers[i].SetOutputLimits(PID_OUT_MIN, PID_OUT_MAX);
    pidControllers[i].SetSampleTime(PID_SAMPLE_TIME_MS);
  }
}

// ============================================================
// Flussmessung mit Tiefpass-Filter + Fehler-Fallback (pro Pumpe)
// ============================================================
float getFilteredFlow(uint8_t pumpIdx) {
  muxSelect(PUMP_SENSOR_CHANNEL[pumpIdx]);
  delay(1);

  float raw, temp;
  if (!sensorRead(raw, temp)) {
    fehlercount++;
    pumpSensorOk[pumpIdx] = false;
#if DEBUG_SERIAL
    Serial.print("Sensor-Lesefehler Pumpe "); Serial.print(pumpIdx + 1);
    Serial.print(" (Mux-Kanal "); Serial.print(PUMP_SENSOR_CHANNEL[pumpIdx]);
    Serial.print("), Fehler #"); Serial.println(fehlercount);
#endif
    return lastGoodFlow[pumpIdx];   // letzter guter Wert, Regler läuft weiter
  }

  if (raw < 0) raw = 0;
  pumpSensorOk[pumpIdx] = true;

  float filtered = FLOW_FILTER_ALPHA * raw + (1.0f - FLOW_FILTER_ALPHA) * lastGoodFlow[pumpIdx];
  lastGoodFlow[pumpIdx] = filtered;
  return filtered;
}

// ============================================================
// Eine Pumpe pro Regelzyklus aktualisieren (PID + Highdriver)
// ============================================================
void updatePump(uint8_t pumpIdx) {
  float setpoint = PUMP_SETPOINT_ULMIN[pumpIdx];

  if (setpoint <= 0.0f) {
    bPumpState[pumpIdx] = false;
    Highdriver4_setvoltage();   // Pumpe bleibt/wird auf 0 V gesetzt
    pidOutput[pumpIdx] = 0;
    return;
  }

  float flow = getFilteredFlow(pumpIdx);

  pidSetpoint[pumpIdx] = setpoint;
  pidInput[pumpIdx]    = flow;
  pidControllers[pumpIdx].Compute();

  bPumpState[pumpIdx] = true;
  Highdriver4_setvoltage(pumpIdx + 1, (uint8_t)(pidOutput[pumpIdx] + 0.5));
}


// ============================================================
// Display: Zellgeometrie (relativ zur sicheren Zone)
// ============================================================
int cellX(int p) { return originX + (p % gridCols) * cellW; }
int cellY(int p) { return gridTop + (p / gridCols) * cellH; }

// ---------------- Draw the parts of the screen that never change ----------------
void drawStaticLayout() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  // Statuszeile oben: laufender I2C-Fehlerzähler (grün=0, orange>0)
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(originX, originY);
  tft.print("Fehler:");
  lastErrStr[0] = '\0';

  for (int p = 0; p < NUM_PUMPS; p++) {
    int x = cellX(p);
    int y = cellY(p);

    tft.drawRect(x, y, cellW, cellH, 0x39C7); // subtle grey border, drawn once

    tft.setTextSize(2);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(x + 4, y + 4);
    tft.print("P");
    tft.print(p + 1);

    // fixed row labels (never repainted)
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(x + 4, y + 26);
    tft.print("Soll:");
    tft.setCursor(x + 4, y + 46);
    tft.print("Ist: ");
    tft.setCursor(x + 4, y + 66);
    tft.print("U:   ");

    // force the dynamic fields to redraw on first update
    lastSollStr[p][0] = '\0';
    lastIstStr[p][0]  = '\0';
    lastUStr[p][0]    = '\0';
  }
}

// ---------------- Redraw one dynamic field only if its text changed ----------------
void updateField(int x, int y, int w, int h, int textSize, uint16_t color,
                  const char *newText, char *lastText, size_t lastTextSize) {
  if (strncmp(newText, lastText, lastTextSize) == 0) return; // unchanged, skip repaint

  tft.fillRect(x, y, w, h, ST77XX_BLACK);
  tft.setTextSize(textSize);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(newText);
  strncpy(lastText, newText, lastTextSize - 1);
  lastText[lastTextSize - 1] = '\0';
}

// ---------------- Status bar: laufender I2C-Fehlerzähler ----------------
void updateStatusBar() {
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)fehlercount);
  uint16_t color = (fehlercount == 0) ? ST77XX_GREEN : ST77XX_ORANGE;
  updateField(originX + 46, originY, 60, STATUS_BAR_H, 1, color, buf,
              lastErrStr, sizeof(lastErrStr));
}

// ---------------- Update only the numbers, cell by cell ----------------
void updateDisplay() {
  char buf[12];

  updateStatusBar();

  for (int p = 0; p < NUM_PUMPS; p++) {
    int x = cellX(p);
    int y = cellY(p);
    int valX = x + 34;  // right after the "Soll:"/"Ist:"/"U:" label

    // Sollfluss [µl/min]
    snprintf(buf, sizeof(buf), "%.1f", PUMP_SETPOINT_ULMIN[p]);
    updateField(valX, y + 26, ROW_W, ROW_H, 1, ST77XX_WHITE, buf,
                lastSollStr[p], sizeof(lastSollStr[p]));

    // Istfluss [µl/min] - grün wenn frischer Messwert, orange wenn Fallback
    snprintf(buf, sizeof(buf), "%.1f", lastGoodFlow[p]);
    uint16_t istColor = pumpSensorOk[p] ? ST77XX_GREEN : ST77XX_ORANGE;
    updateField(valX, y + 46, ROW_W, ROW_H, 1, istColor, buf,
                lastIstStr[p], sizeof(lastIstStr[p]));

    // Steuerspannung [V]
    snprintf(buf, sizeof(buf), "%.1fV", pidOutput[p]);
    updateField(valX, y + 66, ROW_W, ROW_H, 1, ST77XX_YELLOW, buf,
                lastUStr[p], sizeof(lastUStr[p]));
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(200); // kurze Wartezeit, bis alle I2C-Teilnehmer nach Power-On bereit sind

  tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 280);
  tft.setRotation(3); // landscape: width()=280, height()=240

  // Safe area: whole screen minus the rounded-corner margin on every side
  originX = CORNER_SAFE_MARGIN;
  originY = CORNER_SAFE_MARGIN;
  int usableW = tft.width()  - 2 * CORNER_SAFE_MARGIN;
  int usableH = tft.height() - 2 * CORNER_SAFE_MARGIN;

  // I2C-Check direkt auf dem Display (Mux/Treiber/Sensoren) - läuft
  // DIAG_DISPLAY_MS lang, dann geht's automatisch weiter.
  showI2CDiagnostics();

  gridTop = originY + STATUS_BAR_H;

  cellW = usableW / gridCols;
  cellH = (usableH - STATUS_BAR_H) / gridRows;

  // Value fields start at x+34 (right after the "Soll:"/"Ist:"/"U:" labels);
  // keep a small 4px margin before the cell's right border.
  ROW_W = cellW - 34 - 4;

  drawStaticLayout();
  Highdriver_init();
  Highdriver_setfrequency(200);
  initSensorsAndPID();
  lastControlTime = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastControlTime >= PID_SAMPLE_TIME_MS) {
    lastControlTime = now;
    for (uint8_t i = 0; i < NUM_PUMPS; i++) {
      updatePump(i);
    }
    updateDisplay();
  }
}
/*
 * ESP32-C3 Super Mini - Fluss- UND Druck-Datenlogger MIT Display
 * ------------------------------------------------------------------
 * Reiner Datenlogger (Stand 23.10.2026). Die Pumpensteuerung (PID-Regelung
 * und Highdriver4-Treiber an 0x79) ist vollstaendig entfernt - dieser Sketch
 * misst und protokolliert ausschliesslich.
 *
 *  - Sensirion SLF3S-0600F Flusssensoren an Mux 0x74, Ports 1..7
 *  - Honeywell HSCMRRV001PD2A3 Drucksensoren an Mux 0x70, Ports 0..7
 *    (beide Muxe TCA9548A/PCA9548A-Style direkt am Haupt-I2C-Bus)
 *  - Ein Sensorpaar = Flusssensor (Mux 0x74) + Drucksensor (Mux 0x70)
 *    mit derselben Port-Nummer. Port 0 hat nur einen Drucksensor.
 *    Welche Sensoren an welchem Port stecken, steht in FLOW_PRESENT[] und
 *    PRESS_PRESENT[] und kann dort angepasst werden.
 *  - Jedes Paar wird alle OVERSAMPLE_INTERVAL_MS abgetastet (Fluss und Druck
 *    im selben Sweep), alle LOG_INTERVAL_MS wird daraus eine CSV-Zeile auf
 *    die SD-Karte geschrieben
 *  - 240x280 ST7789 SPI-Display, 1.69", LANDSCAPE (width=280 / height=240),
 *    zeigt pro Paar Fluss-Mittelwert, Druck-Mittelwert und die Spreizungen
 *
 * Required libraries (install via Arduino Library Manager):
 *   - Adafruit GFX Library
 *   - Adafruit ST7789 Library
 * (Wire, SPI und SD sind im ESP32-Arduino-Core enthalten)
 *
 * Wiring used by this sketch:
 *   I2C:  SCL -> GPIO0   SDA -> GPIO1   (Mux 0x74 und Mux 0x70 parallel)
 *   SPI:  SCK -> GPIO4   MOSI -> GPIO6   MISO -> GPIO7   CS -> GPIO10 (TFT)
 *   TFT:  DC  -> GPIO2   RST  -> GPIO3
 *   SD:   shares the same SPI bus as the TFT (SCK4/MOSI6/MISO7), own CS -> GPIO5
 *
 * Gleichzeitig offene Mux-Kanaele:
 *   Zum Auslesen eines Paares werden beide Muxe auf denselben Port geschaltet.
 *   Das ist zulaessig, weil Flusssensor (0x08) und Drucksensor (0x28)
 *   unterschiedliche I2C-Adressen haben. Ein Mux, an dessen Port kein Sensor
 *   haengt (z.B. Mux 0x74 bei Paar 0), wird komplett abgeschaltet.
 *
 * Zeitstempel:
 *   Der ESP32-C3 hat keine gepufferte Echtzeituhr, daher wird millis()
 *   (Millisekunden seit dem letzten Reset) als erste CSV-Spalte geloggt.
 *   Da pro Messlauf eine eigene Datei angelegt wird, beginnt jede Datei
 *   sauber bei t ~ 0 - die Zeitachse ist pro Datei eindeutig.
 *   Ueberlauf von millis() erst nach ~49,7 Tagen.
 *
 * SD logging:
 *   Bei jedem Start legt initSD() eine NEUE, durchnummerierte Datei an:
 *     /flowlog_001.csv, /flowlog_002.csv, ... (naechste freie Nummer, max. 999)
 *   Bestehende Dateien werden dabei nie ueberschrieben oder fortgesetzt.
 *   Die laufende Nummer steht oben rechts im Display ("#003") und wird beim
 *   Start auf dem I2C-Check-Screen als voller Dateiname angezeigt.
 *
 *   Eine CSV-Zeile pro LOG_INTERVAL_MS. Erste Spalte millis_ms, danach pro
 *   Port ein Block (Ports aufsteigend). Fluss-Spalten nur, wenn an dem Port
 *   ein Flusssensor steckt, Druck-Spalten nur, wenn ein Drucksensor steckt:
 *     Fluss (7 Spalten):
 *       chN_flow_mean_ulmin  Mittelwert ueber das Fenster
 *       chN_flow_min_ulmin   kleinster Einzelwert im Fenster
 *       chN_flow_max_ulmin   groesster Einzelwert im Fenster
 *       chN_temp_C           Temperatur-Mittelwert (Flusssensor)
 *       chN_n                Anzahl gueltiger Flussmessungen
 *       chN_air              Anzahl Messungen mit Air-in-Line-Flag
 *       chN_hiflow           Anzahl Messungen mit High-Flow-Flag
 *     Druck (5 Spalten):
 *       chN_p_mean_mbar      Differenzdruck-Mittelwert ueber das Fenster
 *       chN_p_min_mbar       kleinster Einzelwert im Fenster
 *       chN_p_max_mbar       groesster Einzelwert im Fenster
 *       chN_p_temp_C         Temperatur-Mittelwert (Drucksensor)
 *       chN_p_n              Anzahl gueltiger Druckmessungen
 *   Bei n = 0 bzw. p_n = 0 bleiben die Messwertfelder leer (-> NaN).
 *   Fehlt die Karte oder schlaegt das Mounten fehl, wird das Logging
 *   stillschweigend uebersprungen (sdReady=false), das Display laeuft weiter.
 *
 * Flicker fix: statisches Layout (Zellrahmen, Label, Einheiten) wird EINMAL
 * in setup() gezeichnet. Die loop() malt nur die Zahlenfelder neu, und auch
 * nur dann, wenn sich der Text seit dem letzten Frame geaendert hat.
 *
 * Rounded-corner safe area:
 *   Panel ist 1.69" Diagonale, 240x280 px -> ~0.1164 mm/px
 *   Ein einheitlicher Rand wird an allen vier Kanten frei gehalten.
 *
 * Flusssensor (Sensirion-Datenblatt LQ_DS_SLF3S-0600F, Abschnitt 4.5.1):
 *     SLF3S-1300F -> 500   (ml/min)^-1
 *     SLF3S-0600F -> 10    (ul/min)^-1   (in use here)
 *   I2C-Adresse 0x08, Start-Kommando 0x3608, 9-Byte-Frame, Temp-Faktor 200.
 *
 * Drucksensor Honeywell HSCMRRV001PD2A3 (TruStability HSC):
 *   001PD -> Differenzdruck +-1 psi (= +-68,95 mbar)
 *   2     -> I2C, Adresse 0x28
 *   A     -> Transferfunktion 10..90 % von 2^14 Counts (1638..14745)
 *   3     -> 3,3 V Versorgung
 *   Kein Start-Kommando noetig, der Sensor misst selbststaendig. Ein Read von
 *   4 Bytes liefert: 2 Statusbits, 14 Bit Druck, 11 Bit Temperatur.
 *   Status 00 = gueltig, 01 = Command Mode, 10 = Stale Data, 11 = Diagnose.
 *   Positiver Druck = Port 1 hoeher als Port 2.
 */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ---------------- Pin definitions ----------------
#define I2C_SCL   0
#define I2C_SDA   1

#define TFT_SCK   4
#define TFT_MOSI  6
#define TFT_MISO  7
#define TFT_CS    10
#define TFT_DC    2
#define TFT_RST   3

#define SD_CS     5   // Micro-SD breakout, shares SPI bus with the TFT

// ---------------- I2C ----------------
// 100 kHz wie bisher. Alle Teilnehmer (SLF3S, HSC, TCA9548A) koennen auch
// 400 kHz - bei sauberen Pull-ups bringt das deutlich mehr Zeitreserve.
#define I2C_CLOCK_HZ  100000

#define MUX_FLOW_ADDR   0x74   // 8-Kanal-Mux Flusssensoren
#define MUX_PRESS_ADDR  0x70   // 8-Kanal-Mux Drucksensoren
#define SENSOR_ADDR     0x08   // SLF3x, feste Adresse
#define PRESS_ADDR      0x28   // HSC...2..., Ausgangstyp "2" = 0x28

// ------------------------------------------------------------
// Sensorpaare: Index i <-> Mux-Port PAIR_PORTS[i] an beiden Muxen
// ------------------------------------------------------------
#define NUM_PAIRS 8
const uint8_t PAIR_PORTS[NUM_PAIRS]    = {     0,    1,    2,    3,    4,    5,    6,    7 };
const bool    FLOW_PRESENT[NUM_PAIRS]  = { false, true, true, true, true, true, true, true };
const bool    PRESS_PRESENT[NUM_PAIRS] = {  false, true, true, true, true, true, true, true };

// ---------------- Flusssensor-Skalierung ----------------
#define FLOW_SCALE_FACTOR  10.0f    // SLF3S-0600F (use 500.0f for SLF3S-1300F)
#define TEMP_SCALE_FACTOR  200.0f   // same across the SLF3x family

// ---------------- Drucksensor-Skalierung (HSCMRRV001PD2A3) ----------------
#define PRESS_OUT_MIN   1638.0f     // 10 % von 16383 Counts
#define PRESS_OUT_MAX   14745.0f    // 90 % von 16383 Counts
#define PRESS_P_MIN     (-1.0f)     // psi
#define PRESS_P_MAX     ( 1.0f)     // psi
#define PSI_TO_MBAR     68.9476f

#define HSC_STATUS_OK     0
#define HSC_STATUS_CMD    1
#define HSC_STATUS_STALE  2
#define HSC_STATUS_DIAG   3

// ---------------- Rounded corner safe area ----------------
#define CORNER_SAFE_MARGIN  10  // px kept clear on every edge

// Debug-Ausgabe über Serial (I2C-Diagnose + Lesefehler). Auf 0 setzen für Produktivbetrieb.
#define DEBUG_SERIAL 0

// ------------------------------------------------------------
// Logging-/Messintervall
// ------------------------------------------------------------
// 500 ms = 2 Zeilen/s -> ca. 518.000 Zeilen bei 3 Tagen Laufzeit (unter der
// Excel-Grenze von 1.048.576 Zeilen). Mit den Druckspalten ca. 260 MB.
const unsigned long LOG_INTERVAL_MS = 500;

// ------------------------------------------------------------
// Oversampling
// ------------------------------------------------------------
// Jedes Paar (Fluss + Druck) wird alle OVERSAMPLE_INTERVAL_MS abgetastet,
// pro CSV-Zeile werden Mittelwert / Min / Max ueber das Fenster geschrieben.
// Ein Sweep ueber 8 Paare dauert bei 100 kHz ca. 20 ms (2 Mux-Writes,
// Settle, 9 Byte Fluss, 4 Byte Druck pro Paar), passt also knapp in 25 ms.
// Wird es zu knapp, sinkt nur die Zahl der Samples pro Zeile (Spalten n/p_n).
const unsigned long OVERSAMPLE_INTERVAL_MS = 25;   // -> bis zu 20 Samples pro Zeile
#define MUX_SETTLE_US  500   // Wartezeit nach dem Umschalten der Muxe

// ------------------------------------------------------------
// SLF3x Signaling Flags (drittes Wort des 9-Byte-Frames, Bytes 6-7)
// ------------------------------------------------------------
#define SLF3X_FLAG_AIR_IN_LINE    0x0001
#define SLF3X_FLAG_HIGH_FLOW      0x0002
#define SLF3X_FLAG_EXP_SMOOTHING  0x0020


// ---------------- Objects ----------------
SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ------------------------------------------------------------
// Akkumulatoren pro Paar (innerhalb eines Logintervalls)
// ------------------------------------------------------------
// Fluss
float    accFlowSum[NUM_PAIRS];
float    accFlowMin[NUM_PAIRS];
float    accFlowMax[NUM_PAIRS];
float    accTempSum[NUM_PAIRS];
uint16_t accCount[NUM_PAIRS];
uint16_t accAirCount[NUM_PAIRS];
uint16_t accHighFlowCount[NUM_PAIRS];
// Druck
float    accPSum[NUM_PAIRS];
float    accPMin[NUM_PAIRS];
float    accPMax[NUM_PAIRS];
float    accPTempSum[NUM_PAIRS];
uint16_t accPCount[NUM_PAIRS];

// Aufbereitete Werte des zuletzt abgeschlossenen Fensters (CSV + Display)
float chFlowMean[NUM_PAIRS]   = {0};   // ul/min
float chTempMean[NUM_PAIRS]   = {0};   // degC (Flusssensor)
float chFlowSpread[NUM_PAIRS] = {0};   // max - min im Fenster
bool  chAir[NUM_PAIRS]        = {false};
bool  chValid[NUM_PAIRS]      = {false};

float chPMean[NUM_PAIRS]      = {0};   // mbar
float chPTempMean[NUM_PAIRS]  = {0};   // degC (Drucksensor)
float chPSpread[NUM_PAIRS]    = {0};   // max - min im Fenster
bool  chPValid[NUM_PAIRS]     = {false};

uint32_t fehlercount      = 0;   // Lesefehler Flusssensoren
uint32_t pressFehlercount = 0;   // Lesefehler / ungueltiger Status Drucksensoren

unsigned long lastSampleTime = 0;
unsigned long lastLogTime    = 0;

// ------------------------------------------------------------
// SD-Logging
// ------------------------------------------------------------
const char* LOG_FILE_PREFIX = "/flowlog_";
const char* LOG_FILE_SUFFIX = ".csv";
#define MAX_LOG_FILES  999

char logFilename[32] = "";
uint16_t logFileIndex = 0;
bool sdReady = false;
uint32_t sdWriteErrorCount = 0;

// last-drawn text per field, so we only repaint what changed
char lastFlowStr[NUM_PAIRS][12];
char lastPressStr[NUM_PAIRS][12];
char lastInfoStr[NUM_PAIRS][24];
bool lastAirState[NUM_PAIRS] = {false};

// ---------------- Layout (computed in setup once rotation is set) ----------------
int gridCols = 2;
int gridRows = 4;   // 2x4 Zellen -> alle 8 belegt (Port 0..7)
int cellW, cellH;
int originX, originY;
int gridTop;

// Zell-Layout (relativ zur Zelle, Zellhoehe ca. 51 px)
const int INFO_X      = 30;   // Spreizung/Warnung rechts neben "Kx"
const int INFO_Y      = 3;
const int INFO_H      = 9;
const int VAL_X       = 3;
const int FLOW_Y      = 13;   // Fluss, Textgroesse 2
const int PRESS_Y     = 31;   // Druck, Textgroesse 2
const int VAL_W       = 84;   // 7 Zeichen in Groesse 2
const int VAL_H       = 16;
const int UNIT_X      = 89;   // Einheit rechts neben dem Wert

const int STATUS_BAR_H = 14;
char lastErrStr[24] = "";
char lastSdErrStr[16] = "";


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

// ---------------- Mux control ----------------
void muxWrite(uint8_t muxAddr, uint8_t mask) {
  Wire.beginTransmission(muxAddr);
  Wire.write(mask);
  Wire.endTransmission();
}

// Beide Muxe auf das Paar i schalten. Ein Mux ohne Sensor an diesem Port
// wird komplett abgeschaltet (Maske 0).
void selectPair(uint8_t i) {
  uint8_t port = PAIR_PORTS[i];
  muxWrite(MUX_FLOW_ADDR,  FLOW_PRESENT[i]  ? (uint8_t)(1 << port) : 0);
  muxWrite(MUX_PRESS_ADDR, PRESS_PRESENT[i] ? (uint8_t)(1 << port) : 0);
}

// ---------------- Flusssensor: start continuous measurement (water) ----------------
bool sensorStartContinuous() {
  Wire.beginTransmission(SENSOR_ADDR);
  Wire.write(0x36);
  Wire.write(0x08);
  return (Wire.endTransmission() == 0);
}

// ---------------- Flusssensor: read one measurement frame ----------------
bool sensorRead(float &flow, float &temp, uint16_t &flags) {
  uint8_t buf[9];
  if (Wire.requestFrom((int)SENSOR_ADDR, 9) != 9) return false;
  for (int i = 0; i < 9; i++) buf[i] = Wire.read();

  if (crc8(&buf[0], 2) != buf[2]) return false;
  if (crc8(&buf[3], 2) != buf[5]) return false;
  if (crc8(&buf[6], 2) != buf[8]) return false;

  int16_t rawFlow = (buf[0] << 8) | buf[1];
  int16_t rawTemp = (buf[3] << 8) | buf[4];

  flow  = rawFlow / FLOW_SCALE_FACTOR;  // ul/min
  temp  = rawTemp / TEMP_SCALE_FACTOR;  // deg C
  flags = ((uint16_t)buf[6] << 8) | buf[7];
  return true;
}

// ---------------- Drucksensor HSC: read pressure + temperature ----------------
// 4 Bytes: [S1 S0 P13..P8] [P7..P0] [T10..T3] [T2..T0 x x x x x]
// Gibt nur bei Status 00 (gueltige, neue Daten) true zurueck.
bool pressureRead(float &p_mbar, float &temp, uint8_t &status) {
  uint8_t buf[4];
  status = 0xFF;
  if (Wire.requestFrom((int)PRESS_ADDR, 4) != 4) return false;
  for (int i = 0; i < 4; i++) buf[i] = Wire.read();

  status = buf[0] >> 6;
  if (status != HSC_STATUS_OK) return false;   // Command Mode / Stale / Diagnose

  uint16_t rawP = ((uint16_t)(buf[0] & 0x3F) << 8) | buf[1];
  uint16_t rawT = ((uint16_t)buf[2] << 3) | (buf[3] >> 5);

  float psi = ((float)rawP - PRESS_OUT_MIN) * (PRESS_P_MAX - PRESS_P_MIN)
              / (PRESS_OUT_MAX - PRESS_OUT_MIN) + PRESS_P_MIN;
  p_mbar = psi * PSI_TO_MBAR;
  temp   = (float)rawT * 200.0f / 2047.0f - 50.0f;
  return true;
}


// ============================================================
// SD-Karte: Init + CSV-Logging
// ============================================================
bool initSD() {
  if (!SD.begin(SD_CS, tftSPI)) {
    return false;
  }

  logFileIndex = 0;
  for (uint16_t n = 1; n <= MAX_LOG_FILES; n++) {
    snprintf(logFilename, sizeof(logFilename), "%s%03u%s",
             LOG_FILE_PREFIX, n, LOG_FILE_SUFFIX);
    if (!SD.exists(logFilename)) {
      logFileIndex = n;
      break;
    }
  }
  if (logFileIndex == 0) {
    logFilename[0] = '\0';
    return false;   // alle 999 Nummern belegt -> Karte aufraeumen
  }

  File f = SD.open(logFilename, FILE_WRITE);
  if (!f) {
    logFilename[0] = '\0';
    logFileIndex = 0;
    return false;
  }

  f.print("millis_ms");
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    uint8_t ch = PAIR_PORTS[i];
    if (FLOW_PRESENT[i]) {
      f.print(",ch"); f.print(ch); f.print("_flow_mean_ulmin");
      f.print(",ch"); f.print(ch); f.print("_flow_min_ulmin");
      f.print(",ch"); f.print(ch); f.print("_flow_max_ulmin");
      f.print(",ch"); f.print(ch); f.print("_temp_C");
      f.print(",ch"); f.print(ch); f.print("_n");
      f.print(",ch"); f.print(ch); f.print("_air");
      f.print(",ch"); f.print(ch); f.print("_hiflow");
    }
    if (PRESS_PRESENT[i]) {
      f.print(",ch"); f.print(ch); f.print("_p_mean_mbar");
      f.print(",ch"); f.print(ch); f.print("_p_min_mbar");
      f.print(",ch"); f.print(ch); f.print("_p_max_mbar");
      f.print(",ch"); f.print(ch); f.print("_p_temp_C");
      f.print(",ch"); f.print(ch); f.print("_p_n");
    }
  }
  f.println();
  f.close();

#if DEBUG_SERIAL
  Serial.print("Logdatei: "); Serial.println(logFilename);
#endif

  return true;
}

// Haengt eine Zeile mit den aktuellen Werten aller Paare an.
// Datei wird pro Aufruf geoeffnet/geschlossen (Schutz bei Stromausfall).
void logToSD(unsigned long timestampMs) {
  if (!sdReady) return;

  File f = SD.open(logFilename, FILE_APPEND);
  if (!f) {
    sdWriteErrorCount++;
    return;
  }

  f.print(timestampMs);
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    if (FLOW_PRESENT[i]) {
      if (accCount[i] > 0) {
        f.print(','); f.print(chFlowMean[i], 2);
        f.print(','); f.print(accFlowMin[i], 2);
        f.print(','); f.print(accFlowMax[i], 2);
        f.print(','); f.print(chTempMean[i], 2);
      } else {
        f.print(",,,,");
      }
      f.print(','); f.print(accCount[i]);
      f.print(','); f.print(accAirCount[i]);
      f.print(','); f.print(accHighFlowCount[i]);
    }
    if (PRESS_PRESENT[i]) {
      if (accPCount[i] > 0) {
        f.print(','); f.print(chPMean[i], 3);
        f.print(','); f.print(accPMin[i], 3);
        f.print(','); f.print(accPMax[i], 3);
        f.print(','); f.print(chPTempMean[i], 1);
      } else {
        f.print(",,,,");
      }
      f.print(','); f.print(accPCount[i]);
    }
  }
  f.println();
  f.close();
}

// ============================================================
// I2C-Diagnose beim Start (beide Muxe, SD, alle Sensorpaare)
// ============================================================
bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

void diagLine(int &y, const char *label, bool ok) {
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(originX, y);
  tft.print(label);
  tft.setTextColor(ok ? ST77XX_GREEN : ST77XX_RED);
  tft.print(ok ? " OK" : " FEHLT");
  y += 14;
}

// Ein Teilergebnis "Fluss OK" / "Druck FEHLT" / "Fluss -" (nicht bestueckt)
void diagPart(int x, int y, const char *name, bool present, bool ok) {
  tft.setCursor(x, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(name);
  if (!present) {
    tft.setTextColor(0x8410);
    tft.print(" -");
  } else {
    tft.setTextColor(ok ? ST77XX_GREEN : ST77XX_RED);
    tft.print(ok ? " OK" : " FEHLT");
  }
}

#define DIAG_DISPLAY_MS 4000

void showI2CDiagnostics() {
  bool muxFlowOk  = i2cPing(MUX_FLOW_ADDR);
  bool muxPressOk = i2cPing(MUX_PRESS_ADDR);

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(originX, originY);
  tft.print("I2C-Check");

  int y = originY + 24;
  diagLine(y, "Mux 0x74 (Fluss):", muxFlowOk);
  diagLine(y, "Mux 0x70 (Druck):", muxPressOk);
  diagLine(y, "SD-Karte:", sdReady);

  tft.setTextSize(1);
  tft.setTextColor(sdReady ? ST77XX_WHITE : 0x8410);
  tft.setCursor(originX, y);
  tft.print("Datei: ");
  tft.print(sdReady ? logFilename : "-");
  y += 14;

  y += 4;

  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    selectPair(i);
    delay(2);
    bool flowOk  = FLOW_PRESENT[i]  && i2cPing(SENSOR_ADDR);
    bool pressOk = PRESS_PRESENT[i] && i2cPing(PRESS_ADDR);

    char label[16];
    snprintf(label, sizeof(label), "Kanal %d:", PAIR_PORTS[i]);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(originX, y);
    tft.print(label);
    diagPart(originX + 66,  y, "Fluss", FLOW_PRESENT[i],  flowOk);
    diagPart(originX + 150, y, "Druck", PRESS_PRESENT[i], pressOk);
    y += 14;

#if DEBUG_SERIAL
    Serial.print(label);
    Serial.print(" Fluss "); Serial.print(FLOW_PRESENT[i]  ? (flowOk  ? "OK" : "FEHLT") : "-");
    Serial.print(" Druck "); Serial.println(PRESS_PRESENT[i] ? (pressOk ? "OK" : "FEHLT") : "-");
#endif
  }

#if DEBUG_SERIAL
  Serial.print("Mux 0x74: "); Serial.println(muxFlowOk  ? "OK" : "FEHLT");
  Serial.print("Mux 0x70: "); Serial.println(muxPressOk ? "OK" : "FEHLT");
#endif

  delay(DIAG_DISPLAY_MS);
}

// ============================================================
// Init: Flusssensoren starten (Drucksensoren brauchen kein Kommando)
// ============================================================
void initFlowSensors() {
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    if (!FLOW_PRESENT[i]) continue;
    selectPair(i);
    delay(2);
    sensorStartContinuous();
    delay(30);
  }
}

// ============================================================
// Oversampling: ein Sweep ueber alle Paare, Werte aufsummieren
// ============================================================
void resetAccumulators() {
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    accFlowSum[i]       = 0.0f;
    accTempSum[i]       = 0.0f;
    accFlowMin[i]       =  1.0e9f;
    accFlowMax[i]       = -1.0e9f;
    accCount[i]         = 0;
    accAirCount[i]      = 0;
    accHighFlowCount[i] = 0;

    accPSum[i]          = 0.0f;
    accPTempSum[i]      = 0.0f;
    accPMin[i]          =  1.0e9f;
    accPMax[i]          = -1.0e9f;
    accPCount[i]        = 0;
  }
}

void sampleAllChannels() {
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    selectPair(i);
    delayMicroseconds(MUX_SETTLE_US);

    // --- Fluss ---
    if (FLOW_PRESENT[i]) {
      float flow, temp;
      uint16_t flags;
      if (sensorRead(flow, temp, flags)) {
        accFlowSum[i] += flow;
        accTempSum[i] += temp;
        if (flow < accFlowMin[i]) accFlowMin[i] = flow;
        if (flow > accFlowMax[i]) accFlowMax[i] = flow;
        accCount[i]++;
        if (flags & SLF3X_FLAG_AIR_IN_LINE) accAirCount[i]++;
        if (flags & SLF3X_FLAG_HIGH_FLOW)   accHighFlowCount[i]++;
      } else {
        fehlercount++;
#if DEBUG_SERIAL
        Serial.print("Fluss-Lesefehler Port "); Serial.print(PAIR_PORTS[i]);
        Serial.print(", Fehler #"); Serial.println(fehlercount);
#endif
      }
    }

    // --- Druck ---
    if (PRESS_PRESENT[i]) {
      float p, pTemp;
      uint8_t status;
      if (pressureRead(p, pTemp, status)) {
        accPSum[i]     += p;
        accPTempSum[i] += pTemp;
        if (p < accPMin[i]) accPMin[i] = p;
        if (p > accPMax[i]) accPMax[i] = p;
        accPCount[i]++;
      } else {
        pressFehlercount++;
#if DEBUG_SERIAL
        Serial.print("Druck-Lesefehler Port "); Serial.print(PAIR_PORTS[i]);
        Serial.print(", Status "); Serial.print(status);
        Serial.print(", Fehler #"); Serial.println(pressFehlercount);
#endif
      }
    }
  }
}

// Fenster abschliessen: Mittelwerte/Spreizung fuer CSV und Display bilden.
// Muss VOR logToSD() und VOR resetAccumulators() laufen.
void finalizeWindow() {
  for (uint8_t i = 0; i < NUM_PAIRS; i++) {
    // Fluss
    if (accCount[i] > 0) {
      chFlowMean[i]   = accFlowSum[i] / accCount[i];
      chTempMean[i]   = accTempSum[i] / accCount[i];
      chFlowSpread[i] = accFlowMax[i] - accFlowMin[i];
      chValid[i]      = true;
    } else {
      chFlowSpread[i] = 0.0f;
      chValid[i]      = false;
    }
    chAir[i] = (accAirCount[i] > 0);

    // Druck
    if (accPCount[i] > 0) {
      chPMean[i]     = accPSum[i] / accPCount[i];
      chPTempMean[i] = accPTempSum[i] / accPCount[i];
      chPSpread[i]   = accPMax[i] - accPMin[i];
      chPValid[i]    = true;
    } else {
      chPSpread[i]   = 0.0f;
      chPValid[i]    = false;
    }
  }
}


// ============================================================
// Display: Zellgeometrie (relativ zur sicheren Zone)
// ============================================================
int cellX(int i) { return originX + (i % gridCols) * cellW; }
int cellY(int i) { return gridTop + (i / gridCols) * cellH; }

// ---------------- Draw the parts of the screen that never change ----------------
void drawStaticLayout() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  // Statuszeile: Lesefehler Fluss/Druck + SD-Schreibfehler + Dateinummer
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(originX, originY);
  tft.print("Fehler:");
  lastErrStr[0] = '\0';

  tft.setCursor(originX + 110, originY);
  tft.print("SD:");
  lastSdErrStr[0] = '\0';

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(originX + 225, originY);
  if (sdReady) {
    char fileBuf[8];
    snprintf(fileBuf, sizeof(fileBuf), "#%03u", logFileIndex);
    tft.print(fileBuf);
  } else {
    tft.print("#---");
  }

  for (int i = 0; i < NUM_PAIRS; i++) {
    int x = cellX(i);
    int y = cellY(i);

    tft.drawRect(x, y, cellW, cellH, 0x39C7);

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(x + 3, y + 3);
    tft.print("K");
    tft.print(PAIR_PORTS[i]);

    tft.setTextColor(0x8410);
    if (FLOW_PRESENT[i]) {
      tft.setCursor(x + UNIT_X, y + FLOW_Y + 4);
      tft.print("ul/min");
    } else {
      tft.setCursor(x + VAL_X, y + FLOW_Y + 4);
      tft.print("kein Fluss");
    }
    if (PRESS_PRESENT[i]) {
      tft.setCursor(x + UNIT_X, y + PRESS_Y + 4);
      tft.print("mbar");
    } else {
      tft.setCursor(x + VAL_X, y + PRESS_Y + 4);
      tft.print("kein Druck");
    }

    lastFlowStr[i][0]  = '\0';
    lastPressStr[i][0] = '\0';
    lastInfoStr[i][0]  = '\0';
  }
}

// ---------------- Redraw one dynamic field only if its text changed ----------------
void updateField(int x, int y, int w, int h, int textSize, uint16_t color,
                  const char *newText, char *lastText, size_t lastTextSize) {
  if (strncmp(newText, lastText, lastTextSize) == 0) return;

  tft.fillRect(x, y, w, h, ST77XX_BLACK);
  tft.setTextSize(textSize);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(newText);
  strncpy(lastText, newText, lastTextSize - 1);
  lastText[lastTextSize - 1] = '\0';
}

// ---------------- Status bar ----------------
void updateStatusBar() {
  // Lesefehler als "Fluss/Druck"
  char buf[24];
  snprintf(buf, sizeof(buf), "%lu/%lu",
           (unsigned long)fehlercount, (unsigned long)pressFehlercount);
  uint16_t color = (fehlercount == 0 && pressFehlercount == 0) ? ST77XX_GREEN
                                                               : ST77XX_ORANGE;
  updateField(originX + 46, originY, 62, STATUS_BAR_H, 1, color, buf,
              lastErrStr, sizeof(lastErrStr));

  char sdBuf[16];
  uint16_t sdColor;
  if (!sdReady) {
    snprintf(sdBuf, sizeof(sdBuf), "FEHLT");
    sdColor = ST77XX_RED;
  } else {
    snprintf(sdBuf, sizeof(sdBuf), "%lu", (unsigned long)sdWriteErrorCount);
    sdColor = (sdWriteErrorCount == 0) ? ST77XX_GREEN : ST77XX_ORANGE;
  }
  updateField(originX + 130, originY, 90, STATUS_BAR_H, 1, sdColor, sdBuf,
              lastSdErrStr, sizeof(lastSdErrStr));
}

// ---------------- Update only the numbers, cell by cell ----------------
// Zelle:  K1  dQ12 dP0.35
//         123.4      ul/min    (gruen / rot = Luft / orange = veraltet)
//         -12.34     mbar      (weiss / orange = veraltet)
void updateDisplay() {
  char buf[24];

  updateStatusBar();

  for (int i = 0; i < NUM_PAIRS; i++) {
    int x = cellX(i);
    int y = cellY(i);

    // --- Fluss-Mittelwert ---
    if (FLOW_PRESENT[i]) {
      if (chAir[i] != lastAirState[i]) {
        lastFlowStr[i][0] = '\0';
        lastAirState[i]   = chAir[i];
      }
      uint16_t flowColor = chAir[i] ? ST77XX_RED
                         : (chValid[i] ? ST77XX_GREEN : ST77XX_ORANGE);
      snprintf(buf, sizeof(buf), "%.1f", chFlowMean[i]);
      updateField(x + VAL_X, y + FLOW_Y, VAL_W, VAL_H, 2, flowColor, buf,
                  lastFlowStr[i], sizeof(lastFlowStr[i]));
    }

    // --- Druck-Mittelwert ---
    if (PRESS_PRESENT[i]) {
      uint16_t pColor = chPValid[i] ? ST77XX_WHITE : ST77XX_ORANGE;
      snprintf(buf, sizeof(buf), "%.2f", chPMean[i]);
      // Farbwechsel ohne Textaenderung trotzdem neu zeichnen
      static bool lastPValid[NUM_PAIRS] = {false};
      if (chPValid[i] != lastPValid[i]) {
        lastPressStr[i][0] = '\0';
        lastPValid[i] = chPValid[i];
      }
      updateField(x + VAL_X, y + PRESS_Y, VAL_W, VAL_H, 2, pColor, buf,
                  lastPressStr[i], sizeof(lastPressStr[i]));
    }

    // --- Infozeile oben: Spreizungen bzw. Warnungen ---
    char fPart[12] = "";
    char pPart[14] = "";
    bool anyInvalid = false;
    if (FLOW_PRESENT[i]) {
      if (chAir[i])         snprintf(fPart, sizeof(fPart), "LUFT");
      else if (!chValid[i]) { snprintf(fPart, sizeof(fPart), "Q--"); anyInvalid = true; }
      else                  snprintf(fPart, sizeof(fPart), "dQ%.0f", chFlowSpread[i]);
    }
    if (PRESS_PRESENT[i]) {
      if (!chPValid[i])     { snprintf(pPart, sizeof(pPart), "P--"); anyInvalid = true; }
      else                  snprintf(pPart, sizeof(pPart), "dP%.2f", chPSpread[i]);
    }
    snprintf(buf, sizeof(buf), "%s%s%s", fPart,
             (fPart[0] && pPart[0]) ? " " : "", pPart);

    uint16_t infoColor = (FLOW_PRESENT[i] && chAir[i]) ? ST77XX_RED
                       : (anyInvalid ? ST77XX_ORANGE : 0x8410);
    // Farbwechsel ohne Textaenderung trotzdem neu zeichnen
    static uint16_t lastInfoColor[NUM_PAIRS] = {0};
    if (infoColor != lastInfoColor[i]) {
      lastInfoStr[i][0] = '\0';
      lastInfoColor[i] = infoColor;
    }
    updateField(x + INFO_X, y + INFO_Y, cellW - INFO_X - 3, INFO_H, 1, infoColor,
                buf, lastInfoStr[i], sizeof(lastInfoStr[i]));
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  delay(200); // I2C-Teilnehmer nach Power-On bereit werden lassen (HSC: ~3 ms)

  // Beide Muxe definiert abschalten
  muxWrite(MUX_FLOW_ADDR, 0);
  muxWrite(MUX_PRESS_ADDR, 0);

  tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 280);
  tft.setRotation(3); // landscape: width()=280, height()=240

  sdReady = initSD();

  originX = CORNER_SAFE_MARGIN;
  originY = CORNER_SAFE_MARGIN;
  int usableW = tft.width()  - 2 * CORNER_SAFE_MARGIN;
  int usableH = tft.height() - 2 * CORNER_SAFE_MARGIN;

  showI2CDiagnostics();

  gridTop = originY + STATUS_BAR_H;
  cellW = usableW / gridCols;                     // 130 px
  cellH = (usableH - STATUS_BAR_H) / gridRows;    // 51 px

  drawStaticLayout();

  initFlowSensors();
  resetAccumulators();
  lastSampleTime = millis();
  lastLogTime    = millis();
}

void loop() {
  unsigned long now = millis();

  // 1) Oversampling: alle OVERSAMPLE_INTERVAL_MS ein Sweep ueber alle Paare
  if (now - lastSampleTime >= OVERSAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    sampleAllChannels();
  }

  // 2) Alle LOG_INTERVAL_MS eine CSV-Zeile aus dem Fenster schreiben
  if (now - lastLogTime >= LOG_INTERVAL_MS) {
    lastLogTime = now;

    finalizeWindow();
    logToSD(now);
    updateDisplay();
    resetAccumulators();
  }
}

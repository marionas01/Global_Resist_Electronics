#define I2C_SLF3S_ADRESS (0x08)   //Defaultadress for Sensirion SLF3S Flow-Sensor
#define SCALE_FACTOR_1300 500.0f
#define SCALE_FACTOR_0600 10000.0f
#define PN_1300 0x07030200
#define PN_0600 0x07030300

float SCALE_FACTOR_FLOW = 1.0;
byte calibration_cmdByte = 0x08;    //default cmdByte for water measurement

void Serial_println(const char str[]) {
  Serial.println(str);

}
void Serial_print(const char str[]) {
  Serial.print(str);
  }

void Serial_println(unsigned char val, int base = DEC) {
  Serial.println(val,base);
 }

void Serial_print(unsigned char val, int base = DEC) {
  Serial.print(val,base);
}

void Serial_println(int val, int base = DEC) {
  Serial.println(val,base);
 }

void Serial_print(int val, int base = DEC) {
  Serial.print(val,base);
}


bool startFlowMeasurement(void) {
  uint32_t ProductNumber;
  int ret;
  // if ((sensors & 0x02)>0) {   //Check if sensor isn't already running
    //Stop continuous measurement
    Wire.beginTransmission(I2C_SLF3S_ADRESS);
    Wire.write(0x3F);
    Wire.write(0xF9);
    ret = Wire.endTransmission();
  // }
  delay(100);
  //Read Product Identifier and Serial number
  Wire.beginTransmission(I2C_SLF3S_ADRESS);
  Wire.write(0x36);
  Wire.write(0x7C);
  ret = Wire.endTransmission();

  Wire.beginTransmission(I2C_SLF3S_ADRESS);
  Wire.write(0xE1);
  Wire.write(0x02);
  ret = Wire.endTransmission();

  Wire.requestFrom(I2C_SLF3S_ADRESS, 6);
  delay(50);
  ProductNumber = Wire.read();
  ProductNumber = ProductNumber<<8 | Wire.read();
  Wire.read();  //CRC
  ProductNumber = ProductNumber<<8 | Wire.read();
  ProductNumber = ProductNumber<<8 | Wire.read();
  Wire.read();  //CRC
  ret = Wire.endTransmission();

//  Serial.print("PN: ");
//  Serial.println(ProductNumber, HEX);

  if ((ProductNumber & 0xFFFFFF00) == PN_1300) {
    SCALE_FACTOR_FLOW = SCALE_FACTOR_1300;
  } else if ((ProductNumber & 0xFFFFFF00) == PN_0600) {
    SCALE_FACTOR_FLOW = SCALE_FACTOR_0600;
  } else {
    SCALE_FACTOR_FLOW = 1;
    return false;
  }
  // Soft reset the sensor
  Wire.beginTransmission(I2C_SLF3S_ADRESS);
  Wire.write(0x06);
  ret = Wire.endTransmission();
  delay(100); // wait long enough for chip reset to complete

  //Start continuous measurement
  Wire.beginTransmission(I2C_SLF3S_ADRESS);  //starting measurement
  Wire.write(0x36); //Start continuous measurement
  //Wire.write(0x08);
  Wire.write(calibration_cmdByte); //Water calibration (0x15 for IPA)
  ret = Wire.endTransmission();
  return true;
}

void stopFlowMeasurement() {
  int ret;
  // if ((sensors & 0x02)>0 || (sensors & 0x04)>0) {
    //Stop continuous measurement
    Wire.beginTransmission(I2C_SLF3S_ADRESS);
    Wire.write(0x3F);
    Wire.write(0xF9);
    ret = Wire.endTransmission();
    delay(100);
//  }
}

bool startFlowMeasurement_simple(void) {
  // Soft reset the sensor
  Wire.beginTransmission(I2C_SLF3S_ADRESS);
  Wire.write(0x06);
  Wire.endTransmission();
  delay(50); // wait long enough for chip reset to complete

  //Start continuous measurement
  Wire.beginTransmission(I2C_SLF3S_ADRESS);  //starting measurement
  Wire.write(0x36); //Start continuous measurement
  //Wire.write(0x08);
  Wire.write(calibration_cmdByte); //Water calibration (0x15 for IPA)
  return (Wire.endTransmission()!=2); //0=success; 3 is already running; 2 is no response
}

void Serial_print(const char str[]);
void Serial_println(const char str[]);

// GEÄNDERT für Standalone-Betrieb:
// readFlow() gibt den Flusswert jetzt direkt als float zurück, statt ihn
// über Serial auszugeben. So kann der PID-Regler im .ino direkt damit
// rechnen, ohne den Umweg über die (jetzt entfernte) Serial-Kommandostrecke.
//
// Rückgabe: Flusswert in µl/min, oder NAN, wenn der I2C-Read fehlgeschlagen ist
// (z. B. Sensor nicht erreichbar / falscher Mux-Kanal). Der Aufrufer sollte
// auf NAN prüfen (isnan()) und in diesem Fall auf den letzten guten Wert
// zurückfallen (siehe getFilteredFlow() im .ino).
float readFlow(void) {
  uint8_t nBytes = Wire.requestFrom(I2C_SLF3S_ADRESS, 3);
  if (nBytes < 3) {
    return NAN;   // Sensor nicht erreichbar -> Fehler an Aufrufer melden
  }

  uint16_t sensor_flow_value;
  int16_t signed_flow_value;
  float scaled_flow_value;

  sensor_flow_value  = Wire.read() << 8;    // read the MSB from the sensor
  sensor_flow_value |= Wire.read();         // read the LSB from the sensor
  Wire.read();  //CRC

  //SCALE_FACTOR_FLOW = SCALE_FACTOR_0600;
  SCALE_FACTOR_FLOW = 10.0f;
  signed_flow_value = (int16_t) sensor_flow_value;
  scaled_flow_value = ((float) signed_flow_value) / SCALE_FACTOR_FLOW;

  return scaled_flow_value;
}

bool startConductivityMeasurement(void) {
  //Start measurement
  Wire.beginTransmission(I2C_SLF3S_ADRESS);  //starting measurement
  Wire.write(0x36); //Start continuous measurement
  Wire.write(0x46);
  return (Wire.endTransmission()!=2); //0=success; 3 is already running; 2 is no response
}

void readConductivity(void) {
  uint16_t sensor_tc_value;
  uint8_t CRC1;
  uint16_t sensor_temp_value;
  uint8_t CRC2;
  uint16_t sensor_dt_value;
  uint8_t CRC3;
  char t[10];
  int16_t signed_temp_value;
  float scaled_temp_value;
  int16_t signed_dt_value;
  float scaled_dt_value;

  Wire.requestFrom(I2C_SLF3S_ADRESS, 9);
//  while (Wire.available() < 9) { }
  sensor_tc_value  = Wire.read() << 8;    // read the MSB from the sensor
  sensor_tc_value |= Wire.read();         // read the LSB from the sensor
  CRC1 = Wire.read();
  sensor_temp_value  = Wire.read() << 8;    // read the MSB from the sensor
  sensor_temp_value |= Wire.read();         // read the LSB from the sensor
  CRC2 = Wire.read();
  sensor_dt_value  = Wire.read() << 8;    // read the MSB from the sensor
  sensor_dt_value |= Wire.read();         // read the LSB from the sensor
  CRC3 = Wire.read();
  Wire.endTransmission();

  signed_temp_value = (int16_t) sensor_temp_value;
  scaled_temp_value = ((float) signed_temp_value) / 200.0f;
  signed_dt_value = (int16_t) sensor_dt_value;
  scaled_dt_value = ((float) signed_dt_value) / 1000.0f;

/*  itoa(sensor_tc_value,t,10);
  Serial_print("Conductivity=");
  Serial_print(t);
  dtostrf(scaled_temp_value,5,3,t);
  Serial_print(" Temperature=");
  Serial_print(t);
  dtostrf(scaled_dt_value,5,3,t);
  Serial_print(" Delta=");
  Serial_println(t);*/
  itoa(sensor_tc_value,t,10);
  //dtostrf(scaled_temp_value,5,3,t);
  Serial_print("C=");
  Serial_println(t);
}

void Liquid_select(uint8_t _liquid) {   //Only Sensirion Sensors for now

  switch(_liquid)
  {
    case 0:
      // set cmd byte for the SLF3S to water calibration
      calibration_cmdByte = 0x08;
      // if ((sensors & 0x02 ) > 0){
      //   startFlowMeasurement();
      //   Serial_println("Switched to Water");
      //}
      break;
    case 1:
      // set cmd byte for the SLF3S to IPA calibration
      calibration_cmdByte = 0x15;
      // if ((sensors & 0x02 ) > 0){
      //   startFlowMeasurement();
      //   Serial_println("Switched to IPA");
      // }
      break;
    default:
      break;
  }
}

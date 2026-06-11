/* TFP401 breakout EDID EEPROM writer (Arduino Uno/Nano or ESP32, Arduino IDE).
 *
 * Writes the custom AMBILIGHT EDID - 1280x720 @ 49.94 Hz CVT-RB, 53.00 MHz,
 * DVI-only - into the breakout's DDC EEPROM (24LC02B, I2C addr 0x50).
 *
 * Easiest wiring: the breakout's "EDID EEPROM" STEMMA QT / JST-SH socket
 * (next to the HDMI port). The TFP401's HDMI and its own USB power must be
 * UNPLUGGED while programming; the V+ wire powers the EEPROM and its
 * on-board 10k pull-ups, so the bus level follows whatever you feed V+.
 *
 *   ESP32-S3-Zero (3.3V logic - NEVER feed V+ from 5V/VIN here).
 *   IDE: board "ESP32S3 Dev Module", USB CDC On Boot = Enabled (the S3-Zero
 *   has native USB only; without this the serial monitor stays silent):
 *     Black  GND -> GND      Red    V+  -> 3V3
 *     Blue   SDA -> GP5      Yellow SCL -> GP6
 *   (GPIO21 is the S3-Zero's onboard RGB LED; GPIO22 does not exist on S3.)
 *
 *   Classic ESP32-WROOM (3.3V logic, same V+ warning):
 *     Black  SDA -> GPIO21   Yellow SCL -> GPIO22, V+ -> 3V3
 *
 *   Arduino Uno/Nano (5V logic):
 *     Black  GND -> GND      Red    V+  -> 5V
 *     Blue   SDA -> A4       Yellow SCL -> A5
 *
 * (Alternative without the cable: solder to the SOT-23-5 EEPROM directly -
 * 3-pin side outer pins are SCL/SDA, middle is GND; WP is tied to GND on
 * the board, so writes are always enabled.)
 *
 * Usage: open the serial monitor at 115200. The sketch dumps the current
 * EDID first. Send 'W' to program, it then verifies byte-for-byte.
 */
#include <Wire.h>

#define EDID_ADDR 0x50
#define PAGE_SIZE 8

static const uint8_t edid[128] = {
  0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x04, 0x81, 0x20, 0x07, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x24, 0x01, 0x03, 0x80, 0x0F, 0x0A, 0x78, 0x02, 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26,
  0x0F, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xB4, 0x14, 0x00, 0xA0, 0x50, 0xD0, 0x11, 0x20, 0x30, 0x20,
  0x35, 0x00, 0x6C, 0x44, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x41, 0x4D, 0x42,
  0x49, 0x4C, 0x49, 0x47, 0x48, 0x54, 0x0A, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x17,
  0x4C, 0x1E, 0x2E, 0x06, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2E
};

static void dumpCurrent(void) {
  Serial.println(F("Current EEPROM contents:"));
  for (uint8_t row = 0; row < 128; row += 16) {
    Wire.beginTransmission(EDID_ADDR);
    Wire.write(row);
    if (Wire.endTransmission() != 0) {
      Serial.println(F("  (no ACK from 0x50 - check wiring/power)"));
      return;
    }
    Wire.requestFrom((uint8_t)EDID_ADDR, (uint8_t)16);
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t b = Wire.available() ? Wire.read() : 0xEE;
      if (b < 0x10) Serial.print('0');
      Serial.print(b, HEX);
      Serial.print(' ');
    }
    Serial.println();
  }
}

static bool writeAll(void) {
  for (uint8_t offset = 0; offset < 128; offset += PAGE_SIZE) {
    Wire.beginTransmission(EDID_ADDR);
    Wire.write(offset);
    for (uint8_t i = 0; i < PAGE_SIZE; i++) {
      Wire.write(edid[offset + i]);
    }
    if (Wire.endTransmission() != 0) {
      Serial.print(F("write NACK at offset "));
      Serial.println(offset);
      return false;
    }
    delay(10); /* EEPROM internal write cycle */
  }
  return true;
}

static bool verifyAll(void) {
  Wire.beginTransmission(EDID_ADDR);
  Wire.write((uint8_t)0);
  if (Wire.endTransmission() != 0) {
    return false;
  }
  for (uint8_t offset = 0; offset < 128; offset += 16) {
    Wire.requestFrom((uint8_t)EDID_ADDR, (uint8_t)16);
    for (uint8_t i = 0; i < 16; i++) {
      uint8_t b = Wire.available() ? Wire.read() : 0xEE;
      if (b != edid[offset + i]) {
        Serial.print(F("MISMATCH at "));
        Serial.print(offset + i);
        Serial.print(F(": wrote 0x"));
        Serial.print(edid[offset + i], HEX);
        Serial.print(F(" read 0x"));
        Serial.println(b, HEX);
        return false;
      }
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  Wire.begin(5, 6); /* S3-Zero: SDA=GP5, SCL=GP6 (GP21 is the onboard LED) */
#elif defined(ARDUINO_ARCH_ESP32)
  Wire.begin(21, 22); /* classic ESP32: SDA, SCL */
#else
  Wire.begin();
#endif
  Wire.setClock(100000);
  delay(200);
  Serial.println(F("TFP401 EDID writer - AMBILIGHT 1280x720@49.94 53MHz DVI"));
  dumpCurrent();
  Serial.println(F("Send 'W' to write the new EDID (WP pad must be released)."));
}

void loop() {
  if (Serial.available() && Serial.read() == 'W') {
    Serial.println(F("Writing..."));
    if (writeAll() && verifyAll()) {
      Serial.println(F("OK: EDID written and verified."));
      dumpCurrent();
      Serial.println(F("Re-protect WP, then power-cycle the TFP401."));
    } else {
      Serial.println(F("FAILED - is the WP pad released? Wiring/pull-ups OK?"));
    }
    Serial.println(F("Send 'W' to try again."));
  }
}

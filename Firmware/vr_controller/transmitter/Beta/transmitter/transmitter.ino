/*
 * DIY VR Sağ Kol - Verici (BluePill STM32F103C8T6)
 * 
 * Bağlantılar:
 *   MPU6050/6500 (I2C1): SCL→PB6, SDA→PB7, VCC→3.3V, GND→GND
 *   NRF24L01 (SPI1):     SCK→PA5, MISO→PA6, MOSI→PA7, CSN→PA4, CE→PB0
 *   Joystick:             VRx→PA0, VRy→PA1, SW→PB10
 *   Tetik pot (20K):      Signal→PA3
 * 
 * Board:      Generic STM32F1 series → BluePill F103C8
 * Upload:     STLink veya Serial
 * Kütüphane:  RF24 by TMRh20
 */

#include <Wire.h>
#include <SPI.h>
#include <RF24.h>

// ===== TANIMLAR =====
#ifndef DEG_TO_RAD
  #define DEG_TO_RAD 0.017453292519943295f
#endif

// ===== AYARLANABILIR PARAMETRELER =====
#define LOOP_INTERVAL_US    5000    // 5ms = 200Hz hedef
#define MADGWICK_BETA       0.08f
#define JOY_DEADZONE        30
#define ANALOG_SMOOTH_ALPHA 0.3f
#define GYRO_CAL_SAMPLES    1000
#define NRF_CHANNEL         76
#define SERIAL_DEBUG        1
#define DEBUG_INTERVAL_MS   2000

// ===== PINLER =====
#define NRF_CE   PB0
#define NRF_CSN  PA4
#define JOY_X    PA0
#define JOY_Y    PA1
#define JOY_SW   PB10
#define TRIGGER  PA3

// ===== NRF24 =====
RF24 radio(NRF_CE, NRF_CSN);
const uint64_t pipe = 0xF0F0F0F0E1LL;

// ===== MPU =====
#define MPU_ADDR     0x68
#define WHO_AM_I     0x75
#define PWR_MGMT_1   0x6B
#define CONFIG_REG   0x1A
#define GYRO_CONFIG  0x1B
#define ACCEL_CONFIG 0x1C
#define ACCEL_XOUT_H 0x3B

// ===== QUATERNION =====
float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
float gxOff = 0.0f, gyOff = 0.0f, gzOff = 0.0f;
unsigned long lastTime = 0;

// ===== ANALOG SMOOTH =====
float smoothJoyX = 0.0f, smoothJoyY = 0.0f, smoothTrig = 0.0f;

// ===== PAKET =====
// !! ALICI VE DRIVER ILE BIREBIR AYNI YAPI !!
// Toplam 24 byte (packed)
struct __attribute__((packed)) Packet {
  float qw, qx, qy, qz;     // 16 byte
  int16_t joyX, joyY;        // 4 byte
  int16_t trigger;            // 2 byte
  uint8_t buttons;            // 1 byte
  uint8_t seq;                // 1 byte
};                            // = 24 byte

Packet pkt;
uint8_t seqCounter = 0;

// ===== ISTATISTIK =====
#if SERIAL_DEBUG
  unsigned long lastPrint = 0;
  uint16_t okCount = 0, failCount = 0;
#endif

// ===========================================================================
//                          MPU FONKSİYONLARI
// ===========================================================================

void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mpuRead(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  return Wire.read();
}

void mpuReadBytes(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, len);
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = Wire.read();
  }
}

bool initMPU() {
  mpuWrite(PWR_MGMT_1, 0x80);
  delay(150);

  uint8_t id = mpuRead(WHO_AM_I);
  Serial.print("WHO_AM_I: 0x");
  Serial.println(id, HEX);

  if (id == 0x70 || id == 0x71) {
    Serial.println("MPU6500 tespit edildi.");
  } else if (id == 0x68) {
    Serial.println("MPU6050 tespit edildi.");
  } else {
    Serial.print("MPU BULUNAMADI! WHO_AM_I=0x");
    Serial.println(id, HEX);
    return false;
  }

  mpuWrite(PWR_MGMT_1, 0x01);
  delay(10);
  mpuWrite(0x6C, 0x00);
  mpuWrite(CONFIG_REG, 0x03);     // DLPF ~42Hz
  mpuWrite(GYRO_CONFIG, 0x08);    // ±500°/s
  mpuWrite(ACCEL_CONFIG, 0x08);   // ±4g

  delay(50);
  uint8_t dummy[14];
  for (int i = 0; i < 20; i++) {
    mpuReadBytes(ACCEL_XOUT_H, dummy, 14);
    delay(2);
  }
  return true;
}

void calibrateGyro() {
  Serial.print("Gyro kalibrasyonu (HAREKET ETME!)");
  int32_t sx = 0, sy = 0, sz = 0;

  for (int i = 0; i < GYRO_CAL_SAMPLES; i++) {
    uint8_t buf[6];
    mpuReadBytes(0x43, buf, 6);
    sx += (int16_t)(buf[0] << 8 | buf[1]);
    sy += (int16_t)(buf[2] << 8 | buf[3]);
    sz += (int16_t)(buf[4] << 8 | buf[5]);
    delay(2);
    if (i % 200 == 0) Serial.print(".");
  }

  gxOff = (float)sx / GYRO_CAL_SAMPLES;
  gyOff = (float)sy / GYRO_CAL_SAMPLES;
  gzOff = (float)sz / GYRO_CAL_SAMPLES;

  Serial.print(" OK (offset: ");
  Serial.print(gxOff, 1); Serial.print(", ");
  Serial.print(gyOff, 1); Serial.print(", ");
  Serial.print(gzOff, 1); Serial.println(")");
}

// ===========================================================================
//                          MADGWICK FİLTRESİ
// ===========================================================================

void madgwick(float gx, float gy, float gz,
              float ax, float ay, float az, float dt) {

  float q0 = qw, q1 = qx, q2 = qy, q3 = qz;

  float norm = sqrtf(ax * ax + ay * ay + az * az);
  if (norm < 0.001f) return;
  float invNorm = 1.0f / norm;
  ax *= invNorm; ay *= invNorm; az *= invNorm;

  float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1;
  float _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
  float _4q0 = 4.0f * q0, _4q1 = 4.0f * q1, _4q2 = 4.0f * q2;
  float _8q1 = 8.0f * q1, _8q2 = 8.0f * q2;
  float q0q0 = q0 * q0, q1q1 = q1 * q1;
  float q2q2 = q2 * q2, q3q3 = q3 * q3;

  float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
  float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay
             - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
  float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay
             - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
  float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;

  norm = sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
  if (norm < 0.001f) return;
  invNorm = 1.0f / norm;
  s0 *= invNorm; s1 *= invNorm; s2 *= invNorm; s3 *= invNorm;

  float qDot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz) - MADGWICK_BETA * s0;
  float qDot1 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy) - MADGWICK_BETA * s1;
  float qDot2 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx) - MADGWICK_BETA * s2;
  float qDot3 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx) - MADGWICK_BETA * s3;

  q0 += qDot0 * dt;
  q1 += qDot1 * dt;
  q2 += qDot2 * dt;
  q3 += qDot3 * dt;

  norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
  invNorm = 1.0f / norm;
  qw = q0 * invNorm;
  qx = q1 * invNorm;
  qy = q2 * invNorm;
  qz = q3 * invNorm;
}

// ===========================================================================
//                          YARDIMCI FONKSİYONLAR
// ===========================================================================

int16_t applyDeadzone(int16_t val, int16_t deadzone) {
  if (val > -deadzone && val < deadzone) return 0;
  return val;
}

float emaFilter(float prev, float raw, float alpha) {
  return prev + alpha * (raw - prev);
}

// ===========================================================================
//                              SETUP
// ===========================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== VR Verici Basliyor ===");
  Serial.print("Paket boyutu: ");
  Serial.print(sizeof(Packet));
  Serial.println(" byte");

  pinMode(JOY_SW, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(400000);

  if (!initMPU()) {
    Serial.println("!!! MPU HATA — Sistem durduruluyor.");
    while (1) delay(1000);
  }
  calibrateGyro();

  Serial.print("NRF24 baslatiliyor... ");
  if (!radio.begin()) {
    Serial.println("BASARISIZ! Kablo baglantisini kontrol et.");
    while (1) delay(1000);
  }
  Serial.println("OK");

  // --- NRF24 AYARLARI — ALICI ILE BIREBIR AYNI OLMALI ---
  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_1MBPS);          // 1Mbps — daha guvenilir
  radio.setChannel(NRF_CHANNEL);          // Kanal 76
  radio.setPayloadSize(sizeof(Packet));   // 24 byte
  radio.setRetries(3, 5);                 // 1000µs aralik, max 5 deneme
  radio.setAutoAck(true);
  radio.openWritingPipe(pipe);
  radio.stopListening();

  Serial.print("Kanal: "); Serial.println(radio.getChannel());
  Serial.print("DataRate: 1MBPS");
  Serial.println("\n=== Verici Hazir ===\n");

  smoothJoyX = (float)analogRead(JOY_X);
  smoothJoyY = (float)analogRead(JOY_Y);
  smoothTrig = (float)analogRead(TRIGGER);

  lastTime = micros();
}

// ===========================================================================
//                              LOOP
// ===========================================================================

void loop() {
  unsigned long loopStart = micros();

  unsigned long now = loopStart;
  unsigned long elapsed = now - lastTime;
  if (elapsed > 1000000UL) elapsed = LOOP_INTERVAL_US;
  float dt = elapsed / 1000000.0f;
  lastTime = now;

  // MPU Okuma
  uint8_t raw[14];
  mpuReadBytes(ACCEL_XOUT_H, raw, 14);

  float ax = (int16_t)(raw[0] << 8 | raw[1]) / 8192.0f;
  float ay = (int16_t)(raw[2] << 8 | raw[3]) / 8192.0f;
  float az = (int16_t)(raw[4] << 8 | raw[5]) / 8192.0f;

  float gx = ((int16_t)(raw[8]  << 8 | raw[9])  - gxOff) / 65.5f * DEG_TO_RAD;
  float gy = ((int16_t)(raw[10] << 8 | raw[11]) - gyOff) / 65.5f * DEG_TO_RAD;
  float gz = ((int16_t)(raw[12] << 8 | raw[13]) - gzOff) / 65.5f * DEG_TO_RAD;

  madgwick(gx, gy, gz, ax, ay, az, dt);

  // Joystick (EMA + deadzone)
  smoothJoyX = emaFilter(smoothJoyX, (float)analogRead(JOY_X), ANALOG_SMOOTH_ALPHA);
  smoothJoyY = emaFilter(smoothJoyY, (float)analogRead(JOY_Y), ANALOG_SMOOTH_ALPHA);
  pkt.joyX = applyDeadzone(map((int)smoothJoyX, 0, 4095, -512, 512), JOY_DEADZONE);
  pkt.joyY = applyDeadzone(map((int)smoothJoyY, 0, 4095, -512, 512), JOY_DEADZONE);

  // Tetik (EMA)
  smoothTrig = emaFilter(smoothTrig, (float)analogRead(TRIGGER), ANALOG_SMOOTH_ALPHA);
  pkt.trigger = map((int)smoothTrig, 0, 4095, 0, 1000);

  // Butonlar
  pkt.buttons = 0;
  if (digitalRead(JOY_SW) == LOW) pkt.buttons |= 0x01;

  // Quaternion
  pkt.qw = qw;
  pkt.qx = qx;
  pkt.qy = qy;
  pkt.qz = qz;

  // Sıra numarası
  pkt.seq = seqCounter++;

  // Gönder
  bool ok = radio.write(&pkt, sizeof(pkt));

  // Debug
  #if SERIAL_DEBUG
    if (ok) okCount++; else failCount++;

    if (millis() - lastPrint >= DEBUG_INTERVAL_MS) {
      unsigned long interval = millis() - lastPrint;
      float totalPkts = okCount + failCount;
      float loopHz = (totalPkts / interval) * 1000.0f;

      Serial.print("Hz:"); Serial.print(loopHz, 0);
      Serial.print(" OK:"); Serial.print(okCount);
      Serial.print(" FAIL:"); Serial.print(failCount);
      Serial.print(" Q:["); Serial.print(qw, 2);
      Serial.print(","); Serial.print(qx, 2);
      Serial.print(","); Serial.print(qy, 2);
      Serial.print(","); Serial.print(qz, 2);
      Serial.print("] J:"); Serial.print(pkt.joyX);
      Serial.print(","); Serial.print(pkt.joyY);
      Serial.print(" T:"); Serial.print(pkt.trigger);
      Serial.print(" Seq:"); Serial.println(pkt.seq);

      okCount = 0;
      failCount = 0;
      lastPrint = millis();
    }
  #endif

  // Sabit döngü hızı
  unsigned long loopDuration = micros() - loopStart;
  if (loopDuration < LOOP_INTERVAL_US) {
    delayMicroseconds(LOOP_INTERVAL_US - loopDuration);
  }
}

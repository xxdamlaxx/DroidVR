/*
 * DIY VR Sağ Kol - Verici (BluePill STM32F103C8T6)
 * 
 * Bağlantılar:
 *   MPU6500 (I2C1): SCL→PB6, SDA→PB7, VCC→3.3V, GND→GND
 *   NRF24L01 (SPI1): SCK→PA5, MISO→PA6, MOSI→PA7, CSN→PA4, CE→PB0
 *   Joystick: VRx→PA0, VRy→PA1, SW→PB10
 *   Tetik pot (20K): Signal→PA3
 * 
 * Board: Generic STM32F1 series → BluePill F103C8
 * Upload: STLink veya Serial
 * Kütüphane: RF24 by TMRh20
 */

#include <Wire.h>
#include <SPI.h>
#include <RF24.h>

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
float qw = 1, qx = 0, qy = 0, qz = 0;
float gxOff = 0, gyOff = 0, gzOff = 0;
unsigned long lastTime = 0;

// ===== PAKET =====
struct __attribute__((packed)) Packet {
  float qw, qx, qy, qz;
  int16_t joyX, joyY;
  int16_t trigger;
  uint8_t buttons;
};

Packet pkt;

// ===== MPU FONKSIYONLARI =====
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
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
}

bool initMPU() {
  mpuWrite(PWR_MGMT_1, 0x80); // reset
  delay(100);

  uint8_t id = mpuRead(WHO_AM_I);
  Serial.print("WHO_AM_I: 0x");
  Serial.println(id, HEX);

  if (id == 0x70 || id == 0x71) Serial.println("MPU6500 OK");
  else if (id == 0x68) Serial.println("MPU6050 OK");
  else {
    Serial.println("MPU BULUNAMADI! Sahte modul olabilir.");
    return false;
  }

  mpuWrite(PWR_MGMT_1, 0x01); // PLL clock
  delay(10);
  mpuWrite(0x6C, 0x00);       // tum eksenler aktif
  mpuWrite(CONFIG_REG, 0x04); // DLPF 20Hz
  mpuWrite(GYRO_CONFIG, 0x08); // +-500 derece/s
  mpuWrite(ACCEL_CONFIG, 0x08); // +-4g
  return true;
}

void calibrateGyro() {
  Serial.print("Gyro kalibrasyonu");
  float sx = 0, sy = 0, sz = 0;
  for (int i = 0; i < 500; i++) {
    uint8_t buf[6];
    mpuReadBytes(0x43, buf, 6);
    sx += (int16_t)(buf[0] << 8 | buf[1]);
    sy += (int16_t)(buf[2] << 8 | buf[3]);
    sz += (int16_t)(buf[4] << 8 | buf[5]);
    delay(2);
    if (i % 100 == 0) Serial.print(".");
  }
  gxOff = sx / 500.0f;
  gyOff = sy / 500.0f;
  gzOff = sz / 500.0f;
  Serial.println(" OK");
}

// ===== MADGWICK =====
void madgwick(float gx, float gy, float gz, float ax, float ay, float az, float dt) {
  float beta = 0.1f;
  float q0 = qw, q1 = qx, q2 = qy, q3 = qz;

  float norm = sqrtf(ax*ax + ay*ay + az*az);
  if (norm < 0.001f) return;
  ax /= norm; ay /= norm; az /= norm;

  float _2q0 = 2*q0, _2q1 = 2*q1, _2q2 = 2*q2, _2q3 = 2*q3;
  float _4q0 = 4*q0, _4q1 = 4*q1, _4q2 = 4*q2;
  float _8q1 = 8*q1, _8q2 = 8*q2;
  float q0q0 = q0*q0, q1q1 = q1*q1, q2q2 = q2*q2, q3q3 = q3*q3;

  float s0 = _4q0*q2q2 + _2q2*ax + _4q0*q1q1 - _2q1*ay;
  float s1 = _4q1*q3q3 - _2q3*ax + 4*q0q0*q1 - _2q0*ay - _4q1 + _8q1*q1q1 + _8q1*q2q2 + _4q1*az;
  float s2 = 4*q0q0*q2 + _2q0*ax + _4q2*q3q3 - _2q3*ay - _4q2 + _8q2*q1q1 + _8q2*q2q2 + _4q2*az;
  float s3 = 4*q1q1*q3 - _2q1*ax + 4*q2q2*q3 - _2q2*ay;

  norm = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
  if (norm < 0.001f) return;
  s0 /= norm; s1 /= norm; s2 /= norm; s3 /= norm;

  float qDot0 = 0.5f*(-q1*gx - q2*gy - q3*gz);
  float qDot1 = 0.5f*( q0*gx + q2*gz - q3*gy);
  float qDot2 = 0.5f*( q0*gy - q1*gz + q3*gx);
  float qDot3 = 0.5f*( q0*gz + q1*gy - q2*gx);

  q0 += (qDot0 - beta*s0)*dt;
  q1 += (qDot1 - beta*s1)*dt;
  q2 += (qDot2 - beta*s2)*dt;
  q3 += (qDot3 - beta*s3)*dt;

  norm = sqrtf(q0*q0 + q1*q1 + q2*q2 + q3*q3);
  qw = q0/norm; qx = q1/norm; qy = q2/norm; qz = q3/norm;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== VR Verici Basliyor ===");

  pinMode(JOY_SW, INPUT_PULLUP);

  // I2C
  Wire.begin();
  Wire.setClock(400000);

  // MPU
  if (!initMPU()) {
    Serial.println("MPU HATA! Durduruluyor.");
    while (1) delay(1000);
  }
  calibrateGyro();

  // NRF24
  Serial.print("NRF24 baslatiliyor... ");
  if (!radio.begin()) {
    Serial.println("BASARISIZ!");
    while (1) delay(1000);
  }
  Serial.println("OK");

  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_1MBPS);
  radio.setChannel(76);
  radio.setPayloadSize(sizeof(Packet));
  radio.setRetries(3, 5);
  radio.openWritingPipe(pipe);
  radio.stopListening();

  // NRF24 detaylari
  Serial.print("Payload: "); Serial.println(sizeof(Packet));
  Serial.print("Kanal: "); Serial.println(radio.getChannel());
  Serial.println("=== Verici Hazir ===");
  lastTime = micros();
}

// ===== LOOP =====
unsigned long lastPrint = 0;
int okCount = 0, failCount = 0;

void loop() {
  unsigned long now = micros();
  float dt = (now - lastTime) / 1000000.0f;
  lastTime = now;

  // MPU oku
  uint8_t raw[14];
  mpuReadBytes(ACCEL_XOUT_H, raw, 14);

  float ax = (int16_t)(raw[0]<<8|raw[1]) / 8192.0f;
  float ay = (int16_t)(raw[2]<<8|raw[3]) / 8192.0f;
  float az = (int16_t)(raw[4]<<8|raw[5]) / 8192.0f;

  float gx = ((int16_t)(raw[8]<<8|raw[9])  - gxOff) / 65.5f * DEG_TO_RAD;
  float gy = ((int16_t)(raw[10]<<8|raw[11]) - gyOff) / 65.5f * DEG_TO_RAD;
  float gz = ((int16_t)(raw[12]<<8|raw[13]) - gzOff) / 65.5f * DEG_TO_RAD;

  madgwick(gx, gy, gz, ax, ay, az, dt);

  // Joystick
  pkt.joyX = map(analogRead(JOY_X), 0, 4095, -512, 512);
  pkt.joyY = map(analogRead(JOY_Y), 0, 4095, -512, 512);

  // Tetik
  pkt.trigger = map(analogRead(TRIGGER), 0, 4095, 0, 1000);

  // Butonlar
  pkt.buttons = 0;
  if (digitalRead(JOY_SW) == LOW) pkt.buttons |= 0x01;

  // Quaternion
  pkt.qw = qw;
  pkt.qx = qx;
  pkt.qy = qy;
  pkt.qz = qz;

  // Gonder
  bool ok = radio.write(&pkt, sizeof(pkt));
  if (ok) okCount++; else failCount++;

  // Her 2 saniyede durum yazdir
  if (millis() - lastPrint > 2000) {
    Serial.print("OK:"); Serial.print(okCount);
    Serial.print(" FAIL:"); Serial.print(failCount);
    Serial.print(" Q:"); Serial.print(qw,2);
    Serial.print(","); Serial.print(qx,2);
    Serial.print(","); Serial.print(qy,2);
    Serial.print(","); Serial.print(qz,2);
    Serial.print(" J:"); Serial.print(pkt.joyX);
    Serial.print(","); Serial.print(pkt.joyY);
    Serial.print(" T:"); Serial.println(pkt.trigger);
    okCount = 0;
    failCount = 0;
    lastPrint = millis();
  }

  delayMicroseconds(5000);
}

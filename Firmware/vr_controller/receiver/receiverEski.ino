/*
 * DIY VR Sağ Kol - Alıcı (Wemos D1 Mini + E01-ML01DP5)
 * 
 * Bağlantılar:
 *   E01-ML01DP5 (NRF24L01 uyumlu):
 *     SCK  → D5 (GPIO14)
 *     MISO → D6 (GPIO12)
 *     MOSI → D7 (GPIO13)
 *     CSN  → D8 (GPIO15)
 *     CE   → D1 (GPIO5)
 *     VCC  → 3.3V
 *     GND  → GND
 * 
 * Arduino IDE ayarları:
 *   Board: LOLIN(WEMOS) D1 mini
 *   Upload Speed: 921600
 *   CPU Frequency: 80MHz
 * 
 * Kütüphaneler:
 *   - RF24 by TMRh20
 * 
 * PC'ye USB Serial üzerinden veri gönderiyor.
 * Format: $VR,qw,qx,qy,qz,joyX,joyY,trigger,buttons\n
 */

#include <SPI.h>
#include <RF24.h>

// Pin tanımları (Wemos D1 Mini)
#define NRF_CE   5   // D1 = GPIO5
#define NRF_CSN  4   // D2 = GPIO4

RF24 radio(NRF_CE, NRF_CSN);
const uint64_t pipeAddr = 0xF0F0F0F0E1LL;

// Veri paketi (verici ile aynı yapı)
struct __attribute__((packed)) ControllerPacket {
  float qw, qx, qy, qz;
  int16_t joyX, joyY;
  int16_t trigger;
  uint8_t buttons;
};

ControllerPacket pkt;
unsigned long lastReceive = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("VR Alici baslatiliyor...");

  if (!radio.begin()) {
    Serial.println("NRF24 BASARISIZ!");
    while (1) { delay(1000); }
  }

  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_1MBPS);
  radio.setChannel(76);
  radio.setPayloadSize(sizeof(ControllerPacket));
  radio.openReadingPipe(1, pipeAddr);
  radio.startListening();

  Serial.println("Alici hazir, veri bekleniyor...");
}

void loop() {
  if (radio.available()) {
    radio.read(&pkt, sizeof(pkt));
    lastReceive = millis();

    // PC'ye gönder: $VR,qw,qx,qy,qz,joyX,joyY,trigger,buttons
    Serial.print("$VR,");
    Serial.print(pkt.qw, 4); Serial.print(",");
    Serial.print(pkt.qx, 4); Serial.print(",");
    Serial.print(pkt.qy, 4); Serial.print(",");
    Serial.print(pkt.qz, 4); Serial.print(",");
    Serial.print(pkt.joyX);  Serial.print(",");
    Serial.print(pkt.joyY);  Serial.print(",");
    Serial.print(pkt.trigger); Serial.print(",");
    Serial.println(pkt.buttons);
  }

  // 2 saniyeden fazla veri gelmezse uyarı
  if (millis() - lastReceive > 2000 && lastReceive > 0) {
    Serial.println("$VR,TIMEOUT");
    lastReceive = 0;
  }
}

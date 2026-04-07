/*
 * DIY VR Sağ Kol - Alıcı (Wemos D1 Mini + E01-ML01DP5)
 * 
 * Bağlantılar:
 *   E01-ML01DP5 (NRF24L01 uyumlu):
 *     SCK  → D5 (GPIO14)
 *     MISO → D6 (GPIO12)
 *     MOSI → D7 (GPIO13)
 *     CSN  → D2 (GPIO4)    ← ESKİ ÇALIŞAN KABLOLAMA
 *     CE   → D1 (GPIO5)
 *     VCC  → 3.3V
 *     GND  → GND
 * 
 * !! CSN PİNİ: Eski çalışan versiyon GPIO4 (D2) kullanıyordu.
 *    Eğer kablolamayı D8'e (GPIO15) taşıdıysan, aşağıdaki
 *    NRF_CSN değerini 15 olarak değiştir.
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
 * Driver beklenen format: $VR,qw,qx,qy,qz,joyX,joyY,trigger,buttons\n
 */

#include <SPI.h>
#include <RF24.h>

// ==============================================================
// PIN AYARLARI — Kablolaman hangisiyse onu kullan!
// ==============================================================
#define NRF_CE   5    // D1 = GPIO5  (her iki versiyonda da aynı)
#define NRF_CSN  4    // D2 = GPIO4  (eski çalışan kablolama)
// #define NRF_CSN  15   // D8 = GPIO15 (yeni kablolama — gerekiyorsa bunu aç)

RF24 radio(NRF_CE, NRF_CSN);
const uint64_t pipeAddr = 0xF0F0F0F0E1LL;

// Veri paketi — verici (transmitter.ino) ile BİREBİR aynı yapı
// Toplam 24 byte (packed)
struct __attribute__((packed)) ControllerPacket {
  float qw, qx, qy, qz;   // 16 byte
  int16_t joyX, joyY;      //  4 byte
  int16_t trigger;          //  2 byte
  uint8_t buttons;          //  1 byte
  uint8_t seq;              //  1 byte
};                          // = 24 byte

ControllerPacket pkt;
unsigned long lastReceive = 0;

// Paket kaybı takibi
uint8_t lastSeq = 0;
bool firstPacket = true;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("VR Alici baslatiliyor...");

  if (!radio.begin()) {
    Serial.println("NRF24 BASARISIZ! CSN/CE kablolarini kontrol et.");
    while (1) { delay(1000); }
  }

  // --- VERİCİ İLE BİREBİR AYNI AYARLAR ---
  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_1MBPS);                    // Verici de 1Mbps
  radio.setChannel(76);                             // Verici kanal 76
  radio.setPayloadSize(sizeof(ControllerPacket));   // 24 byte
  radio.setAutoAck(true);
  radio.openReadingPipe(1, pipeAddr);
  radio.startListening();

  Serial.print("Paket boyutu: ");
  Serial.print(sizeof(ControllerPacket));
  Serial.println(" byte");
  Serial.print("Kanal: ");
  Serial.println(radio.getChannel());
  Serial.print("CSN pin: GPIO");
  Serial.println(NRF_CSN);
  Serial.println("Alici hazir, veri bekleniyor...");
}

void loop() {
  if (radio.available()) {
    radio.read(&pkt, sizeof(pkt));
    lastReceive = millis();

    // Paket kaybı tespiti
    if (!firstPacket) {
      uint8_t expected = lastSeq + 1;
      if (pkt.seq != expected) {
        uint8_t lost = pkt.seq - expected;
        Serial.print("$VR,LOST,");
        Serial.println(lost);
      }
    }
    lastSeq = pkt.seq;
    firstPacket = false;

    // PC'ye gönder — driver'ın beklediği format:
    // $VR,qw,qx,qy,qz,joyX,joyY,trigger,buttons
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

  // Timeout uyarısı
  if (millis() - lastReceive > 2000 && lastReceive > 0) {
    Serial.println("$VR,TIMEOUT");
    lastReceive = 0;
    firstPacket = true;
  }
}

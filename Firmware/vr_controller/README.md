# DIY VR Sağ Kol Kontrolcü

## Donanım

**Verici (kol):** BluePill STM32F103C8T6
- MPU6500 (I2C): SCL→PB6, SDA→PB7
- NRF24L01 (SPI1): SCK→PA5, MISO→PA6, MOSI→PA7, CSN→PA4, CE→PB0
- Joystick: VRx→PA0, VRy→PA1, SW→PB10
- Tetik pot (20K): Signal→PA3
- VCC→3.3V, GND→GND (hepsi)
- NRF24 VCC-GND arasına 10-100µF kapasitör

**Alıcı (PC):** Wemos D1 Mini + E01-ML01DP5
- SCK→D5, MISO→D6, MOSI→D7, CSN→D8, CE→D1
- VCC→3.3V, GND→GND

## Arduino IDE Kurulumu

### Kütüphaneler (Library Manager'dan kur):
- RF24 by TMRh20

### Verici board ayarları:
- Board: Generic STM32F1 series
- Board part number: BluePill F103C8
- Upload method: STLink veya Serial

### Alıcı board ayarları:
- Board: LOLIN(WEMOS) D1 mini

## SteamVR Sürücü Derleme

1. OpenVR SDK indir: https://github.com/ValveSoftware/openvr → C:\openvr'a çıkar
2. Visual Studio 2022 Community kur (C++ Desktop development workload)
3. CMake kur: https://cmake.org/download/
4. Komut satırında:
```
cd steamvr_driver
mkdir build && cd build
cmake .. -DOPENVR_SDK_DIR="C:/openvr"
cmake --build . --config Release
```
5. `diyvr` klasörünü kopyala: `Steam\steamapps\common\SteamVR\drivers\diyvr`
6. `default.vrsettings` içinde COM port numarasını düzenle (Wemos hangi porta bağlıysa)

## Test

1. Vericiye kodu yükle, Serial Monitor'de "Verici hazir!" mesajını gör
2. Alıcıya kodu yükle, Serial Monitor'de "$VR,..." verilerini gör
3. SteamVR'ı aç, sağ el kontrolcüsü görünmeli
4. Kolu hareket ettirince SteamVR'da dönmeli

## Notlar

- Bu 3DOF (sadece rotasyon). Pozisyonel tracking yok.
- MPU6500 sahte çıkarsa WHO_AM_I 0x70 dönmez, Serial'de hata mesajı görürsün.
- MPU6050 da destekleniyor (0x68 döner), otomatik algılar.
- default.vrsettings'de offset değerleri kontrolcünün SteamVR'daki sabit pozisyonunu belirler.

// cc_hud — ST7789 1.54" 240x240 hello-world (Arduino CLI sketch)
// Board: ESP32-S3R8 (8MB Flash + 8MB PSRAM), Arduino Nano ESP32 clone
// FQBN:  esp32:esp32:esp32s3:PartitionScheme=huge_app,CDCOnBoot=cdc
//
// Wiring (LCD silkscreen -> board pin -> ESP32-S3 GPIO):
//   GND -> GND
//   VCC -> 3V3
//   SCL -> D13  (GPIO48, SPI SCK)
//   SDA -> D11  (GPIO38, SPI MOSI)
//   RST -> D8   (GPIO17)
//   DC  -> D7   (GPIO10)
//   SC  -> D10  (GPIO21)  (CS)
//   BL  -> D9   (GPIO18)

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

constexpr int8_t PIN_MOSI = 38;
constexpr int8_t PIN_SCK  = 48;
constexpr int8_t PIN_CS   = 21;
constexpr int8_t PIN_DC   = 10;
constexpr int8_t PIN_RST  = 17;
constexpr int8_t PIN_BL   = 18;

SPIClass spi(HSPI);
Adafruit_ST7789 tft(&spi, PIN_CS, PIN_DC, PIN_RST);

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println();
  Serial.println("[BOOT] cc_hud Adafruit_ST7789 hello-world");
  Serial.printf("[CHIP] %s rev %d, %lu MHz, flash %lu MB, psram %lu B\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(),
                (unsigned long)ESP.getCpuFreqMHz(),
                (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)),
                (unsigned long)ESP.getPsramSize());

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);
  Serial.println("[BL] HIGH (panel should be lit even before SPI)");

  Serial.printf("[SPI] begin SCK=%d MOSI=%d CS=%d (no MISO)\n",
                PIN_SCK, PIN_MOSI, PIN_CS);
  spi.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  Serial.println("[SPI] begin done");

  Serial.println("[TFT] init(240,240,SPI_MODE0)...");
  tft.init(240, 240, SPI_MODE0);
  Serial.println("[TFT] init done");

  tft.setSPISpeed(10 * 1000 * 1000);  // 10 MHz conservative
  tft.setRotation(0);

  Serial.println("[TFT] fill RED");
  tft.fillScreen(ST77XX_RED);
  delay(600);
  Serial.println("[TFT] fill GREEN");
  tft.fillScreen(ST77XX_GREEN);
  delay(600);
  Serial.println("[TFT] fill BLUE");
  tft.fillScreen(ST77XX_BLUE);
  delay(600);

  Serial.println("[TFT] BLACK + 'HELLO'");
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(4);
  tft.setCursor(20, 90);
  tft.print("HELLO");

  Serial.println("[DONE] -> loop()");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("[tick] %u s, heap=%lu\n",
                  (unsigned)(millis() / 1000),
                  (unsigned long)ESP.getFreeHeap());
  }
}

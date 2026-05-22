// Minimal ST7789 hello-world for cc_hud, using Adafruit_ST7789 (not TFT_eSPI).
// Explicit SPI bus + pin selection so we can see exactly what's happening.
// Target: ESP32-S3R8 (8MB PSRAM, but disabled here) + 1.54" 240x240 IPS ST7789.

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// Board pin (silkscreen) → ESP32-S3 GPIO
constexpr int8_t PIN_MOSI = 38;  // D11 — SDA
constexpr int8_t PIN_SCK  = 48;  // D13 — SCL
constexpr int8_t PIN_CS   = 21;  // D10 — CS (silkscreen "SC")
constexpr int8_t PIN_DC   = 10;  // D7  — DC
constexpr int8_t PIN_RST  = 17;  // D8  — RES
constexpr int8_t PIN_BL   = 18;  // D9  — BLK

// ESP32-S3 has SPI2 (HSPI) and SPI3 (FSPI) freely available.
// Force SPI2 (HSPI) so SPI3 stays free for anything else later.
SPIClass spi(HSPI);
Adafruit_ST7789 tft(&spi, PIN_CS, PIN_DC, PIN_RST);

static void log_chip_info() {
  Serial.printf("[CHIP] model=%s rev=%d cores=%d cpu=%lu Hz\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(),
                (int)ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz() * 1000000UL);
  Serial.printf("[CHIP] flash=%lu MB psram=%lu bytes free_heap=%lu\n",
                (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)),
                (unsigned long)ESP.getPsramSize(),
                (unsigned long)ESP.getFreeHeap());
}

void setup() {
  Serial.begin(115200);
  delay(2500);  // wait for USB CDC enumeration
  Serial.println();
  Serial.println("[BOOT] cc_hud — Adafruit_ST7789 hello-world");
  log_chip_info();

  // Manual backlight first (so we can see something even if SPI fails)
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);
  Serial.println("[BL] HIGH (backlight should be on now — look at the panel)");

  Serial.printf("[SPI] begin SCK=%d MISO=-1 MOSI=%d CS=%d\n",
                PIN_SCK, PIN_MOSI, PIN_CS);
  spi.begin(PIN_SCK, /*MISO*/ -1, PIN_MOSI, PIN_CS);
  Serial.println("[SPI] begin done");

  Serial.println("[TFT] init(240, 240, SPI_MODE0)...");
  tft.init(240, 240, SPI_MODE0);
  Serial.println("[TFT] init done");

  // Slow clock for safety while bringing up — 10 MHz is plenty for 1.54"
  tft.setSPISpeed(10 * 1000 * 1000);

  Serial.println("[TFT] setRotation(0)");
  tft.setRotation(0);

  Serial.println("[TFT] fillScreen RED");
  tft.fillScreen(ST77XX_RED);
  delay(600);

  Serial.println("[TFT] fillScreen GREEN");
  tft.fillScreen(ST77XX_GREEN);
  delay(600);

  Serial.println("[TFT] fillScreen BLUE");
  tft.fillScreen(ST77XX_BLUE);
  delay(600);

  Serial.println("[TFT] BLACK + 'HELLO'");
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(4);
  tft.setCursor(20, 90);
  tft.print("HELLO");

  Serial.println("[DONE] entering loop()");
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

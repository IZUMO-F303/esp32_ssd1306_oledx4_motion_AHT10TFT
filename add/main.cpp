#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <WiFi.h>
#include <time.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// WiFi設定
const char* ssid = "Buffalo-G-7FC8";          // Wi-FiのSSID
const char* password = "3eicprewd5bx6";  // Wi-Fi of password

// 人感センサー設定
const int PIR_PIN = 14;
const unsigned long STARFIELD_TIMEOUT = 1 * 60 * 1000; // 1分でスクリーンセーバー開始 (テスト用)
const unsigned long DISPLAY_OFF_TIMEOUT = 2 * 60 * 1000; // 2分で消灯 (テスト用)
unsigned long lastMotionTime = 0;


enum DisplayState {
  STATE_ON,
  STATE_STARFIELD,
  STATE_OFF
};
DisplayState currentState = STATE_ON;
const int DEFAULT_BRIGHTNESS = 64;

// スターフィールド設定
const int MAX_STARS = 100;
struct Star {
  float x, y, z;
  int px, py;
};
Star stars[MAX_STARS];

// LovyanGFX configuration for ST7789 (240x240)
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
  lgfx::Light_PWM    _light_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 3;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = 18;  // SCL
      cfg.pin_mosi = 23;  // SDA
      cfg.pin_miso = -1;
      cfg.pin_dc   = 2;   // DC (RS)
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           =    -1; // CS not used
      cfg.pin_rst          =     4; // RES (RST)
      cfg.pin_busy         =    -1;
      cfg.panel_width      =   240;
      cfg.panel_height     =   240;
      cfg.offset_x         =     0;
      cfg.offset_y         =     0;
      cfg.offset_rotation  =     0;
      cfg.dummy_read_pixel =     8;
      cfg.dummy_read_bits  =     1;
      cfg.readable         =  true;
      cfg.invert           =  true; // ST7789 often needs inversion
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       =  true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 5; // BLK (LED)
      cfg.invert = false;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX display;
Adafruit_AHTX0 aht;

void initStarfield() {
  for (int i = 0; i < MAX_STARS; i++) {
    stars[i].x = random(-100, 100);
    stars[i].y = random(-100, 100);
    stars[i].z = random(1, 240);
    stars[i].px = -1;
    stars[i].py = -1;
  }
}

void drawStarfield() {
  display.startWrite();
  for (int i = 0; i < MAX_STARS; i++) {
    // 星のサイズを計算 (手前に来るほど大きく: 1〜3ピクセル)
    int size = (stars[i].z < 80) ? 3 : (stars[i].z < 160) ? 2 : 1;

    // 古い星を消去 (サイズに合わせて消去)
    if (stars[i].px >= 0) {
      int prev_size = (stars[i].z + 2 < 80) ? 3 : (stars[i].z + 2 < 160) ? 2 : 1;
      display.fillRect(stars[i].px, stars[i].py, prev_size, prev_size, TFT_BLACK);
    }

    // 星の移動
    stars[i].z -= 2;
    if (stars[i].z <= 1) {
      stars[i].x = random(-100, 100);
      stars[i].y = random(-100, 100);
      stars[i].z = 240;
    }

    // 3D投影
    int sx = (int)(stars[i].x * 120 / stars[i].z) + 120;
    int sy = (int)(stars[i].y * 120 / stars[i].z) + 120;

    if (sx >= 0 && sx < (240 - size) && sy >= 0 && sy < (240 - size)) {
      uint16_t brightness = (uint16_t)((240 - stars[i].z) * 255 / 240);
      uint16_t color = display.color565(brightness, brightness, brightness);
      display.fillRect(sx, sy, size, size, color);
      stars[i].px = sx;
      stars[i].py = sy;
    } else {
      stars[i].px = -1;
    }
  }
  display.endWrite();
}

/**
 * Calculate Absolute Humidity (g/m^3) using Tetens formula
 * @param temp Temperature in Celsius
 * @param hum Relative Humidity in %
 * @return Absolute Humidity in g/m^3
 */
float calculateAbsoluteHumidity(float temp, float hum) {
  // Saturation vapor pressure (hPa) using Tetens formula
  float es = 6.1078f * pow(10.0f, (7.5f * temp) / (temp + 237.3f));
  // Actual vapor pressure (hPa)
  float e = es * (hum / 100.0f);
  // Absolute humidity (g/m^3)
  return 217.0f * (e / (temp + 273.15f));
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Initialising...");

  // Initialize I2C for AHT10
  Wire.begin(17, 16);
  if (!aht.begin()) {
    Serial.println("Could not find AHT10/AHT20 sensor!");
    while (1) delay(10);
  }
  Serial.println("AHT10/AHT20 found");

  // PIRセンサーの初期設定
  pinMode(PIR_PIN, INPUT);
  lastMotionTime = millis();

  display.init();
  display.setRotation(0);
  display.setBrightness(64); // Set brightness to 25%
  display.fillScreen(TFT_BLACK);
  
  // Set Japanese font for title
 // display.setFont(&fonts::lgfxJapanGothic_16);
 // display.setTextColor(TFT_CYAN);
 // display.setTextSize(1.5); // Adjust size for Japanese
 // display.setCursor(10, 10);
 // display.println("環境時計");
  
  // WiFi接続表示
  display.setFont(&fonts::Font4);
  display.setTextSize(1.0);
  display.setTextColor(TFT_YELLOW);
  display.setCursor(10, 45);
  display.print("WiFi connecting...");
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  
  // 接続中表示を消去
  display.fillRect(0, 45, 240, 40, TFT_BLACK); 
  
  // NTP設定 (JST: UTC+9, サマータイムなし)
  configTime(9 * 3600, 0, "ntp.nict.jp", "time.google.com");
  
  // 区切り線
  display.drawFastHLine(0, 110, 240, TFT_WHITE);
  
  Serial.println("Setup complete.");
}

void loop() {
  static int last_min = -1; // 前回の「分」を保持
  unsigned long now = millis();
  unsigned long elapsedSinceMotion = now - lastMotionTime;

  // 人感センサーのチェック
  if (digitalRead(PIR_PIN) == HIGH) {
    lastMotionTime = now;
    if (currentState != STATE_ON) {
      currentState = STATE_ON;
      display.setBrightness(DEFAULT_BRIGHTNESS);
      display.fillScreen(TFT_BLACK);
      // 区切り線（再描画が必要な場合）
      display.drawFastHLine(0, 110, 240, TFT_WHITE);
      last_min = -1; // 表示を即座に更新させる
      Serial.println("Motion detected! Display ON.");
    }
  } else {
    if (currentState == STATE_ON && elapsedSinceMotion > STARFIELD_TIMEOUT) {
      currentState = STATE_STARFIELD;
      display.fillScreen(TFT_BLACK);
      initStarfield();
      Serial.println("Starting Starfield screensaver...");
    } else if (currentState == STATE_STARFIELD && elapsedSinceMotion > DISPLAY_OFF_TIMEOUT) {
      currentState = STATE_OFF;
      display.fillScreen(TFT_BLACK);
      display.setBrightness(0);
      Serial.println("No motion for 10 minutes. Display OFF.");
    }
  }

  // 状態に応じた処理
  if (currentState == STATE_OFF) {
    delay(500);
    return;
  }

  if (currentState == STATE_STARFIELD) {
    drawStarfield();
    delay(20); // アニメーションを滑らかにするため
    return;
  }

  // STATE_ON の場合：通常の時計・環境表示
  struct tm timeinfo;
  bool time_valid = getLocalTime(&timeinfo);

  // 時刻が取得できており、かつ「分」が更新された場合のみ描画
  if (time_valid && timeinfo.tm_min != last_min) {
    last_min = timeinfo.tm_min;

    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);
    float abs_hum = calculateAbsoluteHumidity(temp.temperature, humidity.relative_humidity);

    display.startWrite();
    
    // 時計表示 (JST) - HH:MM
    display.setFont(&fonts::Font7);
    display.setTextSize(1.6);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(0, 10);
    display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    // 温湿度表示用の設定
    display.setFont(&fonts::Font4);
    display.setTextSize(1.5);

    // 温度表示
    display.fillRect(0, 120, 240, 40, TFT_BLACK); 
    display.setTextColor(TFT_ORANGE);
    display.setCursor(10, 120);
    display.printf("%.1f C", temp.temperature);

    // 相対湿度表示
    display.fillRect(0, 160, 240, 40, TFT_BLACK);
    display.setTextColor(TFT_BLUE);
    display.setCursor(10, 160);
    display.printf("%.1f %%", humidity.relative_humidity);

    // 絶対湿度表示
    display.fillRect(0, 200, 240, 40, TFT_BLACK);
    display.setTextColor(TFT_GREEN);
    display.setCursor(10, 200);
    display.printf("%.1f g/m3", abs_hum);

    display.endWrite();

    // シリアル出力 (更新時のみ)
    Serial.printf("%02d:%02d:%02d | Temp: %.2f C, Hum: %.2f %%, Abs: %.2f g/m3\n", 
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                  temp.temperature, humidity.relative_humidity, abs_hum);
  }

  delay(500); // 判定のために短めの間隔でループ
}

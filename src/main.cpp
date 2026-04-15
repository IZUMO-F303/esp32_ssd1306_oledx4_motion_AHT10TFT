#include <Arduino.h>
#include <math.h>

// LovyanGFX v1の日本語フォントを有効にする
#define LGFX_USE_V1_FONT_JP

#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_AHTX0.h>
#include <Preferences.h>
#include "secrets.h"

Preferences preferences;

// 年、月、日から月齢を計算する関数 (Jean Meeus's algorithm)
double calculateMoonAge(int year, int month, int day) {
    // 年、月、日からユリウス日を計算
    if (month < 3) {
        year--;
        month += 12;
    }
    int a = year / 100;
    int b = a / 4;
    int c = 2 - a + b;
    int e = 365.25 * (year + 4716);
    int f = 30.6001 * (month + 1);
    double jd = c + day + e + f - 1524.5;

    // 2000年1月6日の新月(JD 2451550.1)からの日数を計算
    double days_since_new_moon = jd - 2451550.1;

    // 朔望月で割る
    double new_moons = days_since_new_moon / 29.53058867;

    // 小数部分を取り出し、朔望月を掛ける
    double moon_age = (new_moons - floor(new_moons)) * 29.53058867;

    return moon_age;
}


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.nict.jp", 9 * 3600, 60000); // 9時間オフセット (JST), 更新間隔60秒

const char* ssid     = SECRET_SSID;
const char* password = SECRET_PASS;

// --- OpenWeatherMap設定 ---
const char* apiKey = SECRET_API_KEY;
const char* city = SECRET_CITY; // 都市名で指定

// --- タッチスクリーンと省電力設定 ---
const int touchPin = 33;            // タッチ検出に使用するGPIOピン
const int motionPin = 14;           // 人感センサに使用するGPIOピン
const int touchThreshold = 30;      // タッチ検出の閾値 (値が小さいほど敏感)
unsigned long lastInteractionTime = 0; // 最後の操作（タッチまたは人感）時刻
unsigned long lastMotionCheckTime = 0; // 最後に人感センサをチェックした時刻
const unsigned long displayTimeout = 1 * 60 * 1000; // 1分 (ms) でスクリーンセーバー
const unsigned long displayOffTimeout = 2 * 60 * 1000; // 2分 (ms) で消灯
bool isDisplayOff = false;          // ディスプレイがスクリーンセーバー中かどうかのフラグ
bool isBacklightOff = false;        // バックライトが消灯中かどうかのフラグ

// --- スターフィールド設定 ---
const int DISPLAY_TOTAL_WIDTH = 512;  // 全ディスプレイ合計の幅 (OLED用)
const int DISPLAY_TOTAL_HEIGHT = 64; // 全ディスプレイ合計の高さ (OLED用)
const int NUM_STARS = 100;           // 表示する星の数 (OLED用)

const int TFT_WIDTH = 240;
const int TFT_HEIGHT = 240;
const int MAX_TFT_STARS = 100;

struct Star {
  float x, y, z; // 3D座標
  float pz;      // 前のフレームのz座標 (トレイル用)
};

Star stars[NUM_STARS]; // OLED用の星
struct TFTStar {
  float x, y, z;
  int px, py;
};
TFTStar tftStars[MAX_TFT_STARS]; // TFT用の星

unsigned long lastGOLUpdate = 0; // マンデルブロから流用 (名前は後で修正するかも)
unsigned long UpdatestarAdr = 0; // スターフィールドの更新間隔 (ms)
const unsigned long golUpdateInterval = 50; // スターフィールドの更新間隔 (ms)

// 左側ディスプレイ(0x3C)用の設定
class LGFX_Left : public lgfx::LGFX_Device
{
  lgfx::Panel_SSD1306 _panel_instance;
  lgfx::Bus_I2C       _bus_instance;

public:
  LGFX_Left(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.i2c_port = 0;              // 使用するI2Cポート (0 or 1)
      cfg.freq_write = 400000;       // 送信クロック
      cfg.freq_read  = 400000;       // 受信クロック
      cfg.pin_scl = 22;              // SCLピン
      cfg.pin_sda = 21;              // SDAピン
      cfg.i2c_addr = 0x3C;           // I2Cデバイスアドレス
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.panel_width = 128;
      cfg.panel_height = 64;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 2; // ここに 2 を指定するとデフォルトで180度回転します
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// 右側ディスプレイ(0x3D)用の設定
class LGFX_Right : public lgfx::LGFX_Device
{
  lgfx::Panel_SSD1306 _panel_instance;
  lgfx::Bus_I2C       _bus_instance;

public:
  LGFX_Right(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.i2c_port = 0;              // 使用するI2Cポート (0 or 1)
      cfg.freq_write = 400000;       // 送信クロック
      cfg.freq_read  = 400000;       // 受信クロック
      cfg.pin_scl = 22;              // SCLピン
      cfg.pin_sda = 21;              // SDAピン
      cfg.i2c_addr = 0x3D;           // I2Cデバイスアドレス
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.panel_width = 128;
      cfg.panel_height = 64;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 2; // ここに 2 を指定するとデフォルトで180度回転します
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// 2つ目のI2Cバスの左側ディスプレイ(0x3C)用の設定
class LGFX_Left_Bus1 : public lgfx::LGFX_Device
{
  lgfx::Panel_SSD1306 _panel_instance;
  lgfx::Bus_I2C       _bus_instance;

public:
  LGFX_Left_Bus1(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.i2c_port = 1;              // 使用するI2Cポート (0 or 1)
      cfg.freq_write = 400000;       // 送信クロック
      cfg.freq_read  = 400000;       // 受信クロック
      cfg.pin_scl = 16;              // SCLピン
      cfg.pin_sda = 17;              // SDAピン
      cfg.i2c_addr = 0x3C;           // I2Cデバイスアドレス
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.panel_width = 128;
      cfg.panel_height = 64;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 2; // ここに 2 を指定するとデフォルトで180度回転します
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// 2つ目のI2Cバスの右側ディスプレイ(0x3D)用の設定
class LGFX_Right_Bus1 : public lgfx::LGFX_Device
{
  lgfx::Panel_SSD1306 _panel_instance;
  lgfx::Bus_I2C       _bus_instance;

public:
  LGFX_Right_Bus1(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.i2c_port = 1;              // 使用するI2Cポート (0 or 1)
      cfg.freq_write = 400000;       // 送信クロック
      cfg.freq_read  = 400000;       // 受信クロック
      cfg.pin_scl = 16;              // SCLピン
      cfg.pin_sda = 17;              // SDAピン
      cfg.i2c_addr = 0x3D;           // I2Cデバイスアドレス
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.panel_width = 128;
      cfg.panel_height = 64;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 2; // ここに 2 を指定するとデフォルトで180度回転します
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

// ST7789 TFT (240x240) 用の設定
class LGFX_TFT : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
  lgfx::Light_PWM    _light_instance;

public:
  LGFX_TFT(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 3;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = 18;
      cfg.pin_mosi = 23;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           =    -1;
      cfg.pin_rst          =     4;
      cfg.panel_width      =   240;
      cfg.panel_height     =   240;
      cfg.offset_x         =     0;
      cfg.offset_y         =     0;
      cfg.invert           =  true;
      cfg.bus_shared       =  true;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 5;
      cfg.invert = false;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

static LGFX_Left lgfx_left;
static LGFX_Right lgfx_right;
static LGFX_Left_Bus1 lgfx_left_bus1;
static LGFX_Right_Bus1 lgfx_right_bus1;
static LGFX_TFT lgfx_tft;

Adafruit_AHTX0 aht;

// --- 天気履歴設定 ---
const int HISTORY_SIZE = 48; // 30分間隔で24時間分
char historyDesc[HISTORY_SIZE][32];
float historyTemp[HISTORY_SIZE];
int historyCount = 0;
int historyIndex = 0; // 次に書き込む位置

// --- 予報データ保持 ---
float forecastTemp[16]; // 3時間おき、16個で48時間分
bool isShowingCharts = false;
unsigned long chartStartTime = 0;

/**
 * 24時間/48時間の気温グラフを描画する
 */
void drawTemperatureChart(lgfx::LGFX_Device& gfx, float* data, int count, float minT, float maxT, const char* title) {
    gfx.clear();
    gfx.setTextColor(TFT_WHITE, TFT_BLACK);
    gfx.setFont(&fonts::lgfxJapanGothicP_16);
    gfx.setCursor(0, 0);
    gfx.print(title);
    
    if (count < 2) {
        gfx.setCursor(0, 20);
        gfx.println("データ不足");
        return;
    }

    int chart_x = 0;
    int chart_y = 20;
    int chart_w = 128;
    int chart_h = 42;

    float range = maxT - minT;
    if (range < 1.0f) range = 1.0f;

    for (int i = 0; i < count - 1; i++) {
        int x1 = map(i, 0, count - 1, 0, chart_w - 1);
        int y1 = chart_y + chart_h - (int)((data[i] - minT) / range * (chart_h - 1));
        int x2 = map(i + 1, 0, count - 1, 0, chart_w - 1);
        int y2 = chart_y + chart_h - (int)((data[i + 1] - minT) / range * (chart_h - 1));
        // 線を太くするために、1ピクセルずらして2回描画
        gfx.drawLine(x1, y1, x2, y2, TFT_WHITE);
        gfx.drawLine(x1, y1 + 1, x2, y2 + 1, TFT_WHITE);
    }
    
    // 最小・最大気温の表示 (フォントをFont2に拡大し、位置を調整)
    gfx.setFont(&fonts::Font2);
    gfx.setCursor(0, 16);
    gfx.printf("%.1f", maxT);
    gfx.setCursor(0, 48);
    gfx.printf("%.1f", minT);
}

void drawAllCharts() {
    // 共通スケールの算出 (過去24h + 未来48h)
    float gMin = 100.0f, gMax = -100.0f;
    for (int i = 0; i < historyCount; i++) {
        if (historyTemp[i] < gMin) gMin = historyTemp[i];
        if (historyTemp[i] > gMax) gMax = historyTemp[i];
    }
    for (int i = 0; i < 16; i++) {
        if (forecastTemp[i] < gMin) gMin = forecastTemp[i];
        if (forecastTemp[i] > gMax) gMax = forecastTemp[i];
    }
    if (gMax - gMin < 2.0f) { gMax += 1.0f; gMin -= 1.0f; }

    // OLED 2: 過去24時間
    float pastData[HISTORY_SIZE];
    for (int i = 0; i < historyCount; i++) {
        int idx = (historyCount < HISTORY_SIZE) ? i : (historyIndex + i) % HISTORY_SIZE;
        pastData[i] = historyTemp[idx];
    }
    drawTemperatureChart(lgfx_right, pastData, historyCount, gMin, gMax, "過去24h気温");

    // OLED 3: 今後24時間
    drawTemperatureChart(lgfx_left_bus1, &forecastTemp[0], 8, gMin, gMax, "今後24h予報");

    // OLED 4: 24-48時間後
    drawTemperatureChart(lgfx_right_bus1, &forecastTemp[8], 8, gMin, gMax, "24-48h予報");
}

/**
 * 履歴を不揮発性メモリ(NVS)に保存する
 */
void saveHistoryToNVS() {
    preferences.begin("weather", false);
    preferences.putBytes("hDesc", historyDesc, sizeof(historyDesc));
    preferences.putBytes("hTemp", historyTemp, sizeof(historyTemp));
    preferences.putInt("hIdx", historyIndex);
    preferences.putInt("hCnt", historyCount);
    preferences.end();
    Serial.println("History saved to NVS.");
}

/**
 * 履歴を不揮発性メモリ(NVS)から読み込む
 */
void loadHistoryFromNVS() {
    preferences.begin("weather", true);
    // 過去にデータが保存されているか確認 (インデックスキーの存在チェック)
    if (preferences.isKey("hIdx")) {
        preferences.getBytes("hDesc", historyDesc, sizeof(historyDesc));
        preferences.getBytes("hTemp", historyTemp, sizeof(historyTemp));
        historyIndex = preferences.getInt("hIdx", 0);
        historyCount = preferences.getInt("hCnt", 0);
        Serial.println("History loaded from NVS.");
    } else {
        Serial.println("No history found in NVS.");
    }
    preferences.end();
}

/**
 * 2枚目のOLED (lgfx_right) に過去24時間の履歴を表示する
 */
void displayHistory() {
    if (isDisplayOff || isBacklightOff) return; // 画面オフ時は描画しない
    lgfx_right.clear();
    lgfx_right.setCursor(0, 0);
    lgfx_right.setTextColor(TFT_WHITE, TFT_BLACK);
    lgfx_right.setFont(&fonts::lgfxJapanGothicP_20);
    lgfx_right.print("昨日:");
    
    if (historyCount < HISTORY_SIZE) {
        lgfx_right.printf("(%d/%d)\n", historyCount, HISTORY_SIZE);
    } else {
        // historyIndexは「次に書き込む位置」＝「一番古いデータ（24時間前）」を指している
        lgfx_right.println(historyDesc[historyIndex]);
    }

    float past_max = -100.0f;
    float past_min = 100.0f;
    for (int i = 0; i < historyCount; i++) {
        if (historyTemp[i] > past_max) past_max = historyTemp[i];
        if (historyTemp[i] < past_min) past_min = historyTemp[i];
    }

    if (historyCount == 0) {
        past_max = 0;
        past_min = 0;
    }

    char hist_temp_str[32];
    lgfx_right.setFont(&fonts::efontJA_24);
    int h1 = 22; // 1行目の高さ目安
    int h2 = 22; // 2行目の高さ目安
    
    sprintf(hist_temp_str, "Max:%.0f°C", past_max);
    lgfx_right.setCursor(0, h1);
    lgfx_right.println(hist_temp_str);
    
    sprintf(hist_temp_str, "Min:%.0f°C", past_min);
    lgfx_right.setCursor(0, h1 + h2);
    lgfx_right.println(hist_temp_str);
}

/**
 * Calculate Absolute Humidity (g/m^3) using Tetens formula
 */
float calculateAbsoluteHumidity(float temp, float hum) {
  float es = 6.1078f * pow(10.0f, (7.5f * temp) / (temp + 237.3f));
  float e = es * (hum / 100.0f);
  return 217.0f * (e / (temp + 273.15f));
}

// --- 天気情報取得関数 ---
void updateWeather(bool saveHistory) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping weather update.");
        return;
    }
    Serial.println("Updating weather...");
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/forecast?q=" + String(city) + "&appid=" + String(apiKey) + "&units=metric&lang=ja";
    http.begin(url);
    Serial.println("HTTP begin (without CA).");

    int httpCode = http.GET();
    Serial.print("HTTP Code: ");
    Serial.println(httpCode);

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.println("Payload received.");
            
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.c_str());
                if (!isDisplayOff && !isBacklightOff) {
                    // LovyanGFXでのエラー表示
                    lgfx_left_bus1.clear();
                    lgfx_left_bus1.setCursor(0, 0);
                    lgfx_left_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
                    lgfx_left_bus1.println("Jsonパースエラー");
                    lgfx_right_bus1.clear();
                    lgfx_right_bus1.setCursor(0, 0);
                    lgfx_right_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
                    lgfx_right_bus1.println("Jsonパースエラー");
                }
                http.end();
                return;
            }
            
            Serial.println("JSON parsed.");

            // 天気概況の取得
            String today_weather_desc = doc["list"][0]["weather"][0]["description"].as<String>();
            String tomorrow_weather_desc = doc["list"][8]["weather"][0]["description"].as<String>();

            // 今日の最高・最低気温（現時刻から24時間以内）の計算
            float today_temp_max = -100.0f; // 初期値は非常に小さい値
            float today_temp_min = 100.0f;  // 初期値は非常に大きい値
            for (int i = 0; i < 8; ++i) {
                if (doc["list"][i]) {
                    float max_t = doc["list"][i]["main"]["temp_max"].as<float>();
                    float min_t = doc["list"][i]["main"]["temp_min"].as<float>();
                    if (max_t > today_temp_max) today_temp_max = max_t;
                    if (min_t < today_temp_min) today_temp_min = min_t;
                }
            }

            // 明日の最高・最低気温（24時間後から48時間後まで）の計算
            float tomorrow_temp_max = -100.0f;
            float tomorrow_temp_min = 100.0f;
            for (int i = 8; i < 16; ++i) {
                if (doc["list"][i]) {
                    float max_t = doc["list"][i]["main"]["temp_max"].as<float>();
                    float min_t = doc["list"][i]["main"]["temp_min"].as<float>();
                    if (max_t > tomorrow_temp_max) tomorrow_temp_max = max_t;
                    if (min_t < tomorrow_temp_min) tomorrow_temp_min = min_t;
                }
            }

            // --- 予報気温をグラフ用に保存 (48時間分 = 16データ) ---
            for (int i = 0; i < 16; i++) {
                if (doc["list"][i]) {
                    forecastTemp[i] = doc["list"][i]["main"]["temp"].as<float>();
                }
            }
            
            Serial.println("Weather data extracted.");
            Serial.print("Today: "); Serial.print(today_weather_desc); Serial.print(" Max:"); Serial.print(today_temp_max); Serial.print(" Min:"); Serial.println(today_temp_min);
            Serial.print("Tomorrow: "); Serial.print(tomorrow_weather_desc); Serial.print(" Max:"); Serial.print(tomorrow_temp_max); Serial.print(" Min:"); Serial.println(tomorrow_temp_min);

            // --- 履歴の保存 (引数がtrueの場合のみ) ---
            if (saveHistory) {
                strncpy(historyDesc[historyIndex], today_weather_desc.c_str(), 31);
                historyDesc[historyIndex][31] = '\0';
                historyTemp[historyIndex] = doc["list"][0]["main"]["temp"].as<float>();
                historyIndex = (historyIndex + 1) % HISTORY_SIZE;
                if (historyCount < HISTORY_SIZE) historyCount++;
                Serial.println("History saved to RAM.");
                saveHistoryToNVS(); // NVSに永続化
            }

            // --- ここから描画処理 (画面オンかつグラフ表示中でない時のみ) ---
            if (!isDisplayOff && !isBacklightOff && !isShowingCharts) {
                // 2枚目のOLEDに履歴を表示
                displayHistory();

                // 3枚目のOLED (lgfx_left_bus1) に今日の天気を表示
                int margin = -2; // 行間のマージン調整
                lgfx_left_bus1.clear();
                lgfx_left_bus1.setCursor(0, 0);
                lgfx_left_bus1.setFont(&fonts::lgfxJapanGothicP_20);
                lgfx_left_bus1.print("今日:");
                lgfx_left_bus1.println(today_weather_desc);
                char temp_str_today[30];
                int h1 = lgfx_left_bus1.fontHeight() + margin;
                sprintf(temp_str_today, "Max:%.0f°C", today_temp_max);
                lgfx_left_bus1.setFont(&fonts::efontJA_24);
                lgfx_left_bus1.setCursor(0, h1);
                lgfx_left_bus1.println(temp_str_today);
                sprintf(temp_str_today, "Min:%.0f°C", today_temp_min);
                int h2 = lgfx_left_bus1.fontHeight() + margin;
                lgfx_left_bus1.setCursor(0, h1 + h2);
                lgfx_left_bus1.println(temp_str_today);

                // 4枚目のOLED (lgfx_right_bus1) に明日の天気を表示
                lgfx_right_bus1.clear();
                lgfx_right_bus1.setCursor(0, 0);
                lgfx_right_bus1.setFont(&fonts::lgfxJapanGothicP_20);
                lgfx_right_bus1.print("明日:");
                lgfx_right_bus1.println(tomorrow_weather_desc);
                char temp_str_tomorrow[30];
                lgfx_right_bus1.setFont(&fonts::efontJA_24);
                sprintf(temp_str_tomorrow, "Max:%.0f°C", tomorrow_temp_max);
                lgfx_right_bus1.setCursor(0, h1);
                lgfx_right_bus1.println(temp_str_tomorrow);
                sprintf(temp_str_tomorrow, "Min:%.0f°C", tomorrow_temp_min);
                lgfx_right_bus1.setCursor(0, h1 + h2);
                lgfx_right_bus1.println(temp_str_tomorrow);
                
                Serial.println("OLEDs updated.");
            }
        } else {
            if (!isDisplayOff && !isBacklightOff) {
                // LovyanGFXでのエラー表示
                lgfx_left_bus1.clear();
                lgfx_left_bus1.setCursor(0, 0);
                lgfx_left_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
                char http_err_str[20];
                sprintf(http_err_str, "HTTPエラー: %d", httpCode);
                lgfx_left_bus1.println(http_err_str);
                lgfx_right_bus1.clear();
                lgfx_right_bus1.setCursor(0, 0);
                lgfx_right_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
                lgfx_right_bus1.println(http_err_str);
            }
        }
    } else {
        Serial.print("HTTP GET failed, error: ");
        Serial.println(http.errorToString(httpCode).c_str());

        if (!isDisplayOff && !isBacklightOff) {
            // LovyanGFXでのエラー表示
            lgfx_left_bus1.clear();
            lgfx_left_bus1.setCursor(0, 0);
            lgfx_left_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
            lgfx_left_bus1.println("HTTP GET失敗");
            lgfx_right_bus1.clear();
            lgfx_right_bus1.setCursor(0, 0);
            lgfx_right_bus1.setFont(&fonts::lgfxJapanMinchoP_16);
            lgfx_right_bus1.println("HTTP GET失敗");
        }
    }

    http.end();
    Serial.println("updateWeather() finished.");
}

// スターフィールドの星を初期化
void initStars() {
  for (int i = 0; i < NUM_STARS; i++) {
    stars[i].x = random(-DISPLAY_TOTAL_WIDTH / 2, DISPLAY_TOTAL_WIDTH / 2); // 画面中央を(0,0)として
    stars[i].y = random(-DISPLAY_TOTAL_HEIGHT / 2, DISPLAY_TOTAL_HEIGHT / 2); // 画面中央を(0,0)として
    stars[i].z = random(1, DISPLAY_TOTAL_WIDTH / 2); // Z軸の奥行き (遠いほど数値が大きい)
    stars[i].pz = stars[i].z; // 前フレームのZ座標も初期化
  }
  for (int i = 0; i < MAX_TFT_STARS; i++) {
    tftStars[i].x = random(-100, 100);
    tftStars[i].y = random(-100, 100);
    tftStars[i].z = random(1, 240);
    tftStars[i].px = -1;
    tftStars[i].py = -1;
  }
}

// スターフィールドを描画
void drawStarfield() {
  // OLED用の描画
  for (int i = 0; i < NUM_STARS; i++) {
    // 前の星の位置を黒で消す (トレイル)
    float psx = (stars[i].x / stars[i].pz) * (DISPLAY_TOTAL_WIDTH / 2) + (DISPLAY_TOTAL_WIDTH / 2);
    float psy = (stars[i].y / stars[i].pz) * (DISPLAY_TOTAL_HEIGHT / 2) + (DISPLAY_TOTAL_HEIGHT / 2);

    int prev_display_index_x = (int)psx / 128;
    int prev_local_x = (int)psx % 128;
    int prev_local_y = (int)psy;

    int prev_star_size = map((int)stars[i].pz, 1, DISPLAY_TOTAL_WIDTH / 2, 2, 0);
    if (prev_star_size == 0) prev_star_size = 1;

    // 前の星を消去
    switch (prev_display_index_x) {
      case 0: lgfx_left.fillRect(prev_local_x, prev_local_y, prev_star_size, prev_star_size, TFT_BLACK); break;
      case 1: lgfx_right.fillRect(prev_local_x, prev_local_y, prev_star_size, prev_star_size, TFT_BLACK); break;
      case 2: lgfx_left_bus1.fillRect(prev_local_x, prev_local_y, prev_star_size, prev_star_size, TFT_BLACK); break;
      case 3: lgfx_right_bus1.fillRect(prev_local_x, prev_local_y, prev_star_size, prev_star_size, TFT_BLACK); break;
    }


    // スクリーン座標に変換 (新しい位置)
    float sx = (stars[i].x / stars[i].z) * (DISPLAY_TOTAL_WIDTH / 2) + (DISPLAY_TOTAL_WIDTH / 2);
    float sy = (stars[i].y / stars[i].z) * (DISPLAY_TOTAL_HEIGHT / 2) + (DISPLAY_TOTAL_HEIGHT / 2);

    // 画面外に出たら星をリセット
    if (sx < 0 || sx > DISPLAY_TOTAL_WIDTH || sy < 0 || sy > DISPLAY_TOTAL_HEIGHT || stars[i].z < 0.1) {
      stars[i].x = random(-DISPLAY_TOTAL_WIDTH / 2, DISPLAY_TOTAL_WIDTH / 2);
      stars[i].y = random(-DISPLAY_TOTAL_HEIGHT / 2, DISPLAY_TOTAL_HEIGHT / 2);
      stars[i].z = random(1, DISPLAY_TOTAL_WIDTH / 2);
      stars[i].pz = stars[i].z;
    } else {
      // 描画 (新しい星)
      int display_index_x = (int)sx / 128; // 0, 1, 2, 3
      int local_x = (int)sx % 128;
      int local_y = (int)sy;

      // 奥行きに応じて星の大きさを変える
      int star_size = map((int)stars[i].z, 1, DISPLAY_TOTAL_WIDTH / 2, 2, 0);
      if (star_size == 0) star_size = 1; // 最小1ピクセル
      uint16_t color = TFT_WHITE; // 白
      
      switch (display_index_x) {
        case 0: lgfx_left.fillRect(local_x, local_y, star_size, star_size, color); break;
        case 1: lgfx_right.fillRect(local_x, local_y, star_size, star_size, color); break;
        case 2: lgfx_left_bus1.fillRect(local_x, local_y, star_size, star_size, color); break;
        case 3: lgfx_right_bus1.fillRect(local_x, local_y, star_size, star_size, color); break;
      }

      // 星を動かす (z値を減らす)
      stars[i].pz = stars[i].z; // 現在のzを前のzに保存
      stars[i].z -= 0.5; // 速度
    }
  }

  // TFT用の描画
  if (!isBacklightOff) {
    lgfx_tft.startWrite();
    for (int i = 0; i < MAX_TFT_STARS; i++) {
      int size = (tftStars[i].z < 80) ? 3 : (tftStars[i].z < 160) ? 2 : 1;
      if (tftStars[i].px >= 0) {
        int prev_size = (tftStars[i].z + 2 < 80) ? 3 : (tftStars[i].z + 2 < 160) ? 2 : 1;
        lgfx_tft.fillRect(tftStars[i].px, tftStars[i].py, prev_size, prev_size, TFT_BLACK);
      }
      tftStars[i].z -= 2;
      if (tftStars[i].z <= 1) {
        tftStars[i].x = random(-100, 100);
        tftStars[i].y = random(-100, 100);
        tftStars[i].z = 240;
      }
      int sx = (int)(tftStars[i].x * 120 / tftStars[i].z) + 120;
      int sy = (int)(tftStars[i].y * 120 / tftStars[i].z) + 120;
      if (sx >= 0 && sx < (240 - size) && sy >= 0 && sy < (240 - size)) {
        uint16_t brightness = (uint16_t)((240 - tftStars[i].z) * 255 / 240);
        uint16_t color = lgfx_tft.color565(brightness, brightness, brightness);
        lgfx_tft.fillRect(sx, sy, size, size, color);
        tftStars[i].px = sx;
        tftStars[i].py = sy;
      } else {
        tftStars[i].px = -1;
      }
    }
    lgfx_tft.endWrite();
  }
}


/**
 * 月を描画する関数
 * @param x, y : 中心座標
 * @param r    : 半径 (今回は16)
 * @param age  : 月齢 (0.0 〜 29.5)
 */
void drawMoon(int x, int y, int r, float age) {
  // 1. 全体を「影」の色で塗る (黒または暗いグレー)
  lgfx_left.fillCircle(x, y, r, TFT_BLACK); 
  // 輪郭だけ描いておくと新月の時も位置がわかります
  lgfx_left.drawCircle(x, y, r, TFT_DARKGRAY); 

  // 月齢を 0.0〜1.0 の比率に変換 (0:新月, 0.5:満月, 1.0:新月)
  float phase = age / 29.53;
  if (phase > 1.0) phase = 0.0;

  // 左右どちらが光るか判定 (半分より前なら右が光る)
  bool waxing = (phase < 0.5); 
  
  // 光の幅を計算 (-r 〜 +r)
  // cosを使って満ち欠けの「厚み」をシミュレート
  float cp = cos(phase * 2.0 * PI);
  int w = abs(r * cp);

  if (waxing) {
    // 【満ちていくとき】右半分を半円で塗り、左半分を楕円の影or光で調整
    lgfx_left.fillArc(x, y, 0, r, 270, 90, TFT_WHITE); // 右半円
    if (phase < 0.25) {
      // 三日月：左側の楕円で「影」を上書き
      lgfx_left.fillEllipse(x, y, w, r, TFT_BLACK);
    } else {
      // 半月〜満月：左側の楕円で「光」を追加
      lgfx_left.fillEllipse(x, y, w, r, TFT_WHITE);
    }
  } else {
    // 【欠けていくとき】左半分を半円で塗り、右半分を楕円で調整
    lgfx_left.fillArc(x, y, 0, r, 90, 270, TFT_WHITE); // 左半円
    if (phase < 0.75) {
      // 満月〜下弦：右側の楕円で「光」を追加
      lgfx_left.fillEllipse(x, y, w, r, TFT_WHITE);
    } else {
      // 二十六夜〜新月：右側の楕円で「影」を上書き
      lgfx_left.fillEllipse(x, y, w, r, TFT_BLACK);
    }
  }
}
// 1枚目のOLED（日付と曜日・月齢）を表示する関数
void displayDateOfWeek() {
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = localtime(&epochTime);

    char dateStr[20];
    strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", ptm);

    char dayOfWeekStr[10];
    switch (ptm->tm_wday) {
      case 0: strcpy(dayOfWeekStr, "(日)"); break;
      case 1: strcpy(dayOfWeekStr, "(月)"); break;
      case 2: strcpy(dayOfWeekStr, "(火)"); break;
      case 3: strcpy(dayOfWeekStr, "(水)"); break;
      case 4: strcpy(dayOfWeekStr, "(木)"); break;
      case 5: strcpy(dayOfWeekStr, "(金)"); break;
      case 6: strcpy(dayOfWeekStr, "(土)"); break;
      default: strcpy(dayOfWeekStr, ""); break;
    }

    // 月齢を計算
    double moonAge = calculateMoonAge(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

    lgfx_left.clear();
    lgfx_left.setCursor(0, 0);
    lgfx_left.setFont(&fonts::Font4);
    lgfx_left.println(dateStr);
    lgfx_left.setFont(&fonts::lgfxJapanGothicP_24);
    lgfx_left.println(dayOfWeekStr);
    
    // 月齢画像をOLED右側に描画
    drawMoon(110, 40, 16, moonAge);
    char moonAgeStr[10];
    lgfx_left.setCursor(70, 40);
    lgfx_left.setFont(&fonts::efontJA_24);
    lgfx_left.setTextSize(1);
    sprintf(moonAgeStr, "%d", (int)(moonAge + 0.5));
    lgfx_left.println(moonAgeStr);
    lgfx_left.setTextSize(1.0);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("OLED Test");

  // NVSから履歴を読み込み
  loadHistoryFromNVS();

  // 左側ディスプレイの初期化
  if (!lgfx_left.init()) {
    Serial.println("Left display initialization failed");
    return;
  }

  // 右側ディスプレイの初期化
  if (!lgfx_right.init()) {
    Serial.println("Right display initialization failed");
    return;
  }

  // 2つ目のI2Cバスの左側ディスプレイの初期化
  if (!lgfx_left_bus1.init()) {
    Serial.println("Left display (Bus 1) initialization failed");
    return;
  }

  // 2つ目のI2Cバスの右側ディスプレイの初期化
  if (!lgfx_right_bus1.init()) {
    Serial.println("Right display (Bus 1) initialization failed");
    return;
  }
  
  pinMode(motionPin, INPUT); // 人感センサのピンを入力に設定

  // AHT10の初期化 (I2C Bus 1: 17, 16)
  if (!aht.begin(&Wire1)) {
    Serial.println("Could not find AHT10 sensor!");
  } else {
    Serial.println("AHT10 found");
  }

  // TFTの初期化
  lgfx_tft.init();
  lgfx_tft.setRotation(0);
  lgfx_tft.setBrightness(64);
  lgfx_tft.fillScreen(TFT_BLACK);

  lgfx_left.setBrightness(32); // 明るさ設定(max=255)
  lgfx_right.setBrightness(32); // 明るさ設定(max=255)
  lgfx_left_bus1.setBrightness(32); // 明るさ設定(max=255)
  lgfx_right_bus1.setBrightness(32); // 明るさ設定(max=255)
  
  // WiFi接続
  lgfx_left.clear();
  lgfx_left.setCursor(0, 0);
  lgfx_left.setFont(&fonts::Font2); // 小さめのフォントを選択
  lgfx_left.println("Connecting to WiFi");
  lgfx_left.println(""); // 2行目にドットを表示するために改行

  lgfx_tft.setFont(&fonts::Font4);
  lgfx_tft.setTextColor(TFT_YELLOW);
  lgfx_tft.setCursor(10, 45);
  lgfx_tft.print("WiFi connecting...");

  Serial.print("Connecting to WiFi ");
  WiFi.begin(ssid, password);
  int dotCount = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    lgfx_left.print("."); // ドットを追加
    dotCount++;
    if (dotCount > 10) { // 長くなったらリセットして再度表示
      lgfx_left.setCursor(0, lgfx_left.fontHeight() + 2); // 2行目
      lgfx_left.print("          "); // ドットを消す
      lgfx_left.setCursor(0, lgfx_left.fontHeight() + 2); // 2行目
      dotCount = 0;
    }
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  lgfx_left.clear(); // 接続完了後、一度クリアして次の表示に備える
  lgfx_left.setCursor(0, 0);
  lgfx_left.setFont(&fonts::Font2); // フォントをリセット
  lgfx_left.println("WiFi connected.");
  lgfx_left.print("IP: ");
  lgfx_left.println(WiFi.localIP());

  lgfx_tft.fillRect(0, 45, 240, 40, TFT_BLACK); 
  lgfx_tft.drawFastHLine(0, 110, 240, TFT_WHITE);
  
  delay(2000); // しばらく表示
  
  // NTPクライアントの初期化
  timeClient.begin();
  timeClient.update();
  Serial.println("NTP client initialized and time updated.");

  updateWeather(true);
  displayDateOfWeek(); 
  
  randomSeed(analogRead(0)); // 乱数シードを初期化
  initStars(); // スターフィールドを初期化

  lastInteractionTime = millis(); // 最後の操作時刻を初期化
  lastMotionCheckTime = millis(); // 人感センサの最終チェック時刻を初期化
}

void loop() {
  // --- ループ全体で共有するstatic変数 ---
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastWeatherUpdate = 0;
  static int last_day = -1;
  static int last_minute = -1;
  static int last_moon_age = -1;
  static int last_triangle_x_center = -1;
  static int time_display_bottom_y = 30;

  // --- タッチ入力の検出と人感センサのチェック ---
  int touchValue = touchRead(touchPin);
  bool interactionDetected = false;

  // タッチ検出
  if (touchValue < touchThreshold) {
    interactionDetected = true;
  }

  // 人感センサ検出 (1秒おき)
  if (millis() - lastMotionCheckTime > 1000) {
    lastMotionCheckTime = millis();
    if (digitalRead(motionPin) == HIGH) {
      interactionDetected = true;
      Serial.println("Motion detected!");
    }
  }

  if (interactionDetected) {
    lastInteractionTime = millis(); // 最後の操作時刻を更新

    if (isDisplayOff || isBacklightOff) {
      isDisplayOff = false;
      isBacklightOff = false;
      Serial.println("Waking up from screensaver/off.");
      
      // ディスプレイを復帰させ、明るさを設定
      lgfx_left.wakeup();
      lgfx_right.wakeup();
      lgfx_left_bus1.wakeup();
      lgfx_right_bus1.wakeup();
      lgfx_tft.setBrightness(64);

      lgfx_left.init(); // 時刻表示の再初期化
      lgfx_right.init(); // 時刻表示の再初期化
      lgfx_left_bus1.init(); // 時刻表示の再初期化
      lgfx_right_bus1.init(); // 時刻表示の再初期化

      lgfx_left.setBrightness(32);
      lgfx_right.setBrightness(32);
      lgfx_left_bus1.setBrightness(32);
      lgfx_right_bus1.setBrightness(32);
      
      // 全てのディスプレイをクリア
      lgfx_left.clear();
      lgfx_right.clear();
      lgfx_left_bus1.clear();
      lgfx_right_bus1.clear();
      lgfx_tft.fillScreen(TFT_BLACK);
      lgfx_tft.drawFastHLine(0, 110, 240, TFT_WHITE);
      
      // グラフ表示モードを開始
      isShowingCharts = true;
      chartStartTime = millis();
      updateWeather(false); // 最新予報を取得してグラフ用にデータを保存
      drawAllCharts();
      displayDateOfWeek(); // OLED 1は通常通り日付を表示
      
      // 時刻表示を強制的に再描画させる
      last_minute = -1;
      
      return; // 復帰処理を完了
    }
  }

  // --- モードに応じた処理 ---
  if (!isDisplayOff) {
    // === 通常表示モード ===
    
    // グラフ表示の終了判定 (20秒経過)
    if (isShowingCharts && (millis() - chartStartTime > 20000)) {
        isShowingCharts = false;
        lgfx_right.clear();
        lgfx_left_bus1.clear();
        lgfx_right_bus1.clear();
        displayHistory();
        updateWeather(false);
    }

    // タイムアウトをチェックし、スターフィールドモードに移行
    if (millis() - lastInteractionTime > displayTimeout) {
      Serial.println("Timeout. Entering Starfield.");
      isDisplayOff = true;
      isShowingCharts = false; // 強制終了
      initStars(); // スターフィールドを初期化
      
      // 画面を一旦すべてクリア
      lgfx_left.clear();
      lgfx_right.clear();
      lgfx_left_bus1.clear();
      lgfx_right_bus1.clear();
      lgfx_tft.fillScreen(TFT_BLACK);
      
      lastGOLUpdate = millis(); // スターフィールドの初回更新タイミングを設定
      UpdatestarAdr = millis(); // スターフィールドの開始行リセットタイミングを設定
      return; // 次のループサイクルからスターフィールドを開始
    }

    // --- 以下、従来の表示更新処理 ---
    timeClient.update();

    if (millis() - lastDisplayUpdate > 1000) { // 1秒ごとに更新
      lastDisplayUpdate = millis();

      time_t epochTime = timeClient.getEpochTime();
      struct tm *ptm = localtime(&epochTime);

      // 月齢を計算 (四捨五入して整数にする)
      double currentMoonAge = calculateMoonAge(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
      int currentMoonAgeInt = (int)(currentMoonAge + 0.5);

      // 日付の更新、または月齢（整数値）の更新
      if (last_day == -1 || ptm->tm_mday != last_day || currentMoonAgeInt != last_moon_age) {
          displayDateOfWeek();
          last_day = ptm->tm_mday;
          last_moon_age = currentMoonAgeInt;
      }
      
      // 時刻の更新
      char timeStr[20];
      strftime(timeStr, sizeof(timeStr), "%H:%M", ptm);
      int current_minute = ptm->tm_min;
      int indicator_offset = 5;
      int triangle_height = 10;
      int triangle_base_width = 5;


      if (current_minute != last_minute) {
          last_minute = current_minute;

          // --- TFTの更新 (分が変わったタイミング) ---
          sensors_event_t humidity, temp;
          aht.getEvent(&humidity, &temp);
          float abs_hum = calculateAbsoluteHumidity(temp.temperature, humidity.relative_humidity);

          lgfx_tft.startWrite();
          
          // 時計表示 (JST) - HH:MM
          lgfx_tft.setFont(&fonts::Font7);
          lgfx_tft.setTextSize(1.6);
          lgfx_tft.setTextColor(TFT_WHITE, TFT_BLACK);
          lgfx_tft.setCursor(0, 10);
          lgfx_tft.printf("%02d:%02d", ptm->tm_hour, ptm->tm_min);

          // 温湿度表示
          lgfx_tft.setFont(&fonts::Font4);
          lgfx_tft.setTextSize(1.5);

          lgfx_tft.fillRect(0, 120, 240, 40, TFT_BLACK); 
          lgfx_tft.setTextColor(TFT_ORANGE);
          lgfx_tft.setCursor(10, 120);
          lgfx_tft.printf("%.1f C", temp.temperature);

          lgfx_tft.fillRect(0, 160, 240, 40, TFT_BLACK);
          lgfx_tft.setTextColor(TFT_CYAN); // 湿度は水色に変更
          lgfx_tft.setCursor(10, 160);
          lgfx_tft.printf("%.1f %%", humidity.relative_humidity);

          lgfx_tft.fillRect(0, 200, 240, 40, TFT_BLACK);
          // 絶対湿度に応じて色を変更
          uint16_t abs_hum_color;
          if (abs_hum >= 11.0f) {
              abs_hum_color = TFT_GREEN;
          } else if (abs_hum >= 7.0f) {
              abs_hum_color = TFT_YELLOW;
          } else {
              abs_hum_color = TFT_RED;
          }
          lgfx_tft.setTextColor(abs_hum_color);
          lgfx_tft.setCursor(10, 200);
          lgfx_tft.printf("%.1f g/m3", abs_hum);

          lgfx_tft.endWrite();

      }
    }
  } else {
    // === スクリーンセーバー / 消灯モード ===
    if (millis() - lastInteractionTime > displayOffTimeout) {
      if (!isBacklightOff) {
        isBacklightOff = true;
        Serial.println("Display off timeout. Turning off backlight.");
        lgfx_tft.setBrightness(0);
        lgfx_left.sleep();
        lgfx_right.sleep();
        lgfx_left_bus1.sleep();
        lgfx_right_bus1.sleep();
      }
      // return を削除し、下の共通処理（天気更新など）を実行可能にする
      delay(100); 
    } else {
      // === スターフィールドモード (消灯前のみ実行) ===
      if (millis() - lastGOLUpdate > golUpdateInterval) {
        lastGOLUpdate = millis();
        drawStarfield(); // スターフィールドを描画
        if(millis() - UpdatestarAdr > 10000 ) { // 10秒ごとに開始行をリセットして描画の乱れを防止
          UpdatestarAdr = millis();
          lgfx_right.startWrite();
          lgfx_right.writeCommand(0x40);
          lgfx_right.endWrite();
          lgfx_left.startWrite();
          lgfx_left.writeCommand(0x40);
          lgfx_left.endWrite();
          lgfx_left_bus1.startWrite();
          lgfx_left_bus1.writeCommand(0x40);
          lgfx_left_bus1.endWrite();
          lgfx_right_bus1.startWrite();
          lgfx_right_bus1.writeCommand(0x40);
          lgfx_right_bus1.endWrite();
        }
      }
    }
  }

  // --- 共通の更新処理 (画面のオンオフに関わらず実行) ---
  
  // NTP時刻の更新
  timeClient.update();

  // 天気予報の更新 (30分ごと)
  if (millis() - lastWeatherUpdate > 1800000) {
    lastWeatherUpdate = millis();
    updateWeather(true); // 定期更新時は履歴を保存する
  }
}

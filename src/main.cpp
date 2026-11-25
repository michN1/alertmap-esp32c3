// ВЕРСІЯ 2.5.3 · оновлено 26.11.2026 01:22
// (анімації тривоги та спокою, перегляд на карті, оновлення з GitHub, райони)
//
// Карта тривог v2 — один код для ESP32-S3 (два ядра) та ESP32-C3 (одне ядро)
//
//   netTask  — запити до api.alerts.in.ua (TLS) та оновлення з GitHub
//   ledTask  — анімація світлодіодів зі сталою частотою кадрів
//   loop()   — веб-сервер, DNS точки доступу, нагляд за Wi-Fi
//
// На S3 netTask працює на ядрі 0, ledTask і loop() — на ядрі 1.
// На C3 ядро одне, задачі ділять його за пріоритетами (світлодіоди — вищий).
//
// Налаштування: NVS (Preferences), пишуться ТІЛЬКИ при натисканні «Зберегти».
// Стан тривог у флеш не пишеться взагалі.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include "config.h"
#include "locations.h"

#if CONFIG_FREERTOS_UNICORE
  #define CORE_NET 0
  #define CORE_LED 0
#else
  #define CORE_NET 0
  #define CORE_LED 1
#endif

// ======================= Налаштування =======================

struct Settings {
  String ssid, pass, token;
  String   otaRepo;              // "користувач/репозиторій" на GitHub
  bool     otaAuto     = false;  // встановлювати нові версії автоматично
  uint8_t  apiVer      = 2;      // 1 або 2
  uint16_t intervalS   = 20;     // 15..120 с (ліміт API — 12 запитів/хв з IP)
  uint8_t  ledPin      = DEFAULT_LED_PIN;
  uint16_t ledCount    = DEFAULT_LED_COUNT;
  uint8_t  bright      = 90;     // 1..255
  bool     nightOn     = false;
  uint8_t  nightBright = 12;
  uint8_t  nightFrom   = 22;
  uint8_t  nightTo     = 7;
  uint8_t  calmMode    = 0;      // 0 — прапор, 1 — один колір, 2 — вимкнено
  uint8_t  calmLevel   = 35;     // яскравість спокійного стану, %
  uint8_t  alertAnim   = 0;      // анімація тривоги, див. alertLevel()
  uint8_t  calmAnim    = 0;      // анімація спокійного стану, див. calmFactor()
  uint8_t  animSpeed   = 3;      // 1..5, 3 — звичайна швидкість
  bool     changeFx    = true;   // спалах, коли тривога починається або закінчується
  uint32_t cCalm       = 0x00FF60;
  uint32_t cRed        = 0xFF0000;
  uint32_t cYellow     = 0xFF6000;
  uint32_t cPartial    = 0xFFB000;
  uint32_t cError      = 0x8000FF;
  uint32_t cFlagBlue   = 0x0040FF;   // прапор у спокійному стані
  uint32_t cFlagYellow = 0xFFE000;   // лимонний, щоб не зливався з «частково»
  uint16_t ledUid[MAX_LEDS];     // ledUid[led] = UID області чи району, 0 — не прив'язано
};

Settings cfg;
uint8_t ledParent[MAX_LEDS];     // UID області, якщо світлодіод прив'язаний до району; інакше 0
// Положення кожного світлодіода на карті (за областю, до якої він належить)
uint8_t ledX[MAX_LEDS], ledY[MAX_LEDS];   // 0..255: захід→схід, північ→південь
uint8_t ledDist[MAX_LEDS];                // відстань від центру країни, 0..255
uint8_t ledAng[MAX_LEDS];                 // кут навколо центру, 0..255 на повне коло
Preferences prefs;
SemaphoreHandle_t mux;           // захищає рядки в cfg і структуру net

static String stripBearer(String t) {
  t.trim();
  if (t.startsWith("Bearer ") || t.startsWith("bearer ")) t = t.substring(7);
  t.trim();
  return t;
}

// Піни, на які можна вішати стрічку
static bool pinOk(int p) {
#if CONFIG_IDF_TARGET_ESP32C3
  // без 2, 8, 9 (strapping), 12–17 (флеш), 18/19 (USB), 20/21 (UART)
  return p == 0 || p == 1 || (p >= 3 && p <= 7) || p == 10;
#else
  // ESP32-S3: без 0, 3, 45, 46 (strapping), 19/20 (USB), 26–37 (флеш / PSRAM), 43/44 (UART)
  if (p < 1 || p > 48) return false;
  if (p == 3 || p == 19 || p == 20 || p == 43 || p == 44 || p == 45 || p == 46) return false;
  if (p >= 22 && p <= 37) return false;
  return true;
#endif
}

static uint8_t parentOf(uint16_t uid) {
  for (uint16_t i = 0; i < RAION_COUNT; i++)
    if (RAION_PARENT[i].uid == uid) return RAION_PARENT[i].parent;
  return 0;
}

void rebuildParents() {
  for (int i = 0; i < MAX_LEDS; i++) {
    const uint16_t uid = cfg.ledUid[i];
    ledParent[i] = uid ? parentOf(uid) : 0;
    // район бере положення своєї області
    const uint8_t ob = ledParent[i] ? ledParent[i] : (uint8_t)(uid < 32 ? uid : 0);
    ledX[i] = OBLAST_POS[ob][0];
    ledY[i] = OBLAST_POS[ob][1];
    const float dx = (float)ledX[i] - MAP_CENTER_X, dy = (float)ledY[i] - MAP_CENTER_Y;
    const float dist = sqrtf(dx * dx + dy * dy) * 1.6f;
    ledDist[i] = dist > 255 ? 255 : (uint8_t)dist;
    ledAng[i] = (uint8_t)((atan2f(dy, dx) + PI) * (255.0f / (2.0f * PI)));
  }
}

// За замовчуванням: перші 27 світлодіодів — області в порядку старої прошивки
void defaultMap() {
  for (int i = 0; i < MAX_LEDS; i++) cfg.ledUid[i] = (i < 27) ? DEFAULT_OBLAST_UIDS[i] : 0;
}

void loadSettings() {
  prefs.begin("amap", false);
  cfg.ssid        = prefs.getString("ssid", "");
  cfg.pass        = prefs.getString("pass", "");
  cfg.token       = stripBearer(prefs.getString("token", DEFAULT_TOKEN));
  cfg.otaRepo     = prefs.getString("repo", DEFAULT_OTA_REPO);
  cfg.otaAuto     = prefs.getBool("oAuto", false);
  cfg.apiVer      = prefs.getUChar("api", 2);
  cfg.intervalS   = prefs.getUShort("int", 20);
  cfg.ledPin      = prefs.getUChar("pin", DEFAULT_LED_PIN);
  cfg.ledCount    = prefs.getUShort("cnt", DEFAULT_LED_COUNT);
  cfg.bright      = prefs.getUChar("br", 90);
  cfg.nightOn     = prefs.getBool("nOn", false);
  cfg.nightBright = prefs.getUChar("nBr", 12);
  cfg.nightFrom   = prefs.getUChar("nFr", 22);
  cfg.nightTo     = prefs.getUChar("nTo", 7);
  cfg.calmMode    = prefs.getUChar("calm", 0);
  cfg.calmLevel   = prefs.getUChar("calmL", 35);
  cfg.alertAnim   = prefs.getUChar("aAnim", 0);
  cfg.calmAnim    = prefs.getUChar("cAnim", 0);
  cfg.animSpeed   = constrain(prefs.getUChar("aSpd", 3), 1, 5);
  cfg.changeFx    = prefs.getBool("chFx", true);
  cfg.cCalm       = prefs.getUInt("cCalm", cfg.cCalm);
  cfg.cRed        = prefs.getUInt("cRed", cfg.cRed);
  cfg.cYellow     = prefs.getUInt("cYel", cfg.cYellow);
  cfg.cPartial    = prefs.getUInt("cPart", cfg.cPartial);
  cfg.cError      = prefs.getUInt("cErr", cfg.cError);
  cfg.cFlagBlue   = prefs.getUInt("cFlB", cfg.cFlagBlue);
  cfg.cFlagYellow = prefs.getUInt("cFlY", cfg.cFlagYellow);
  defaultMap();
  if (prefs.getBytesLength("umap") == sizeof(cfg.ledUid)) prefs.getBytes("umap", cfg.ledUid, sizeof(cfg.ledUid));
  prefs.end();

  cfg.ledCount  = constrain(cfg.ledCount, 1, MAX_LEDS);
  cfg.intervalS = constrain(cfg.intervalS, 15, 120);
  if (cfg.apiVer != 1 && cfg.apiVer != 2) cfg.apiVer = 2;
  if (!pinOk(cfg.ledPin)) cfg.ledPin = DEFAULT_LED_PIN;
  rebuildParents();
}

void saveSettings() {
  prefs.begin("amap", false);
  prefs.putString("token", cfg.token);
  prefs.putString("repo", cfg.otaRepo);
  prefs.putBool("oAuto", cfg.otaAuto);
  prefs.putUChar("api", cfg.apiVer);
  prefs.putUShort("int", cfg.intervalS);
  prefs.putUChar("pin", cfg.ledPin);
  prefs.putUChar("br", cfg.bright);
  prefs.putBool("nOn", cfg.nightOn);
  prefs.putUChar("nBr", cfg.nightBright);
  prefs.putUChar("nFr", cfg.nightFrom);
  prefs.putUChar("nTo", cfg.nightTo);
  prefs.putUChar("calm", cfg.calmMode);
  prefs.putUChar("calmL", cfg.calmLevel);
  prefs.putUChar("aAnim", cfg.alertAnim);
  prefs.putUChar("cAnim", cfg.calmAnim);
  prefs.putUChar("aSpd", cfg.animSpeed);
  prefs.putBool("chFx", cfg.changeFx);
  prefs.putUInt("cCalm", cfg.cCalm);
  prefs.putUInt("cRed", cfg.cRed);
  prefs.putUInt("cYel", cfg.cYellow);
  prefs.putUInt("cPart", cfg.cPartial);
  prefs.putUInt("cErr", cfg.cError);
  prefs.putUInt("cFlB", cfg.cFlagBlue);
  prefs.putUInt("cFlY", cfg.cFlagYellow);
  prefs.end();
}

void saveMap() {
  prefs.begin("amap", false);
  prefs.putUShort("cnt", cfg.ledCount);
  prefs.putBytes("umap", cfg.ledUid, sizeof(cfg.ledUid));
  prefs.end();
}

void saveWifi() {
  prefs.begin("amap", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.end();
}

// ======================= Стан мережі / даних =======================

struct NetState {
  char     alerts[ALERT_BUF] = {0};   // alerts[uid] = стан локації
  uint16_t len       = 0;
  bool     hasData   = false;
  int      http      = 0;      // останній код відповіді (0 — запитів ще не було)
  uint32_t lastOkMs  = 0;      // останній 200 або 304
  uint32_t okCnt     = 0;
  uint32_t errCnt    = 0;
};
NetState net;

volatile bool otaBusy = false;

static bool isStale(bool hasData, uint32_t lastOkMs, uint16_t intervalS) {
  if (!hasData) return true;
  uint32_t limit = (uint32_t)intervalS * 6000UL;
  if (limit < STALE_MIN_SEC * 1000UL) limit = STALE_MIN_SEC * 1000UL;
  return millis() - lastOkMs > limit;
}

// ======================= Оновлення з GitHub Releases =======================
// У останньому релізі репозиторію мають лежати файли:
//   version.json  {"version":"2.3.1"}
//   firmware-esp32s3.bin / firmware-esp32c3.bin  прошивка для відповідного чипа
//   index.html    веб-інтерфейс (необов'язково)

enum class OtaSt : uint8_t { Idle, Checking, UpToDate, Available, Installing, Error };
struct OtaState {
  OtaSt st = OtaSt::Idle;
  char  latest[16] = "";
  char  msg[72] = "";
};
OtaState ota;                                    // захищено mux
volatile bool otaCheckReq = false;
volatile bool otaInstallReq = false;

static void setOta(OtaSt st, const char *msg = "") {
  xSemaphoreTake(mux, portMAX_DELAY);
  ota.st = st;
  strlcpy(ota.msg, msg, sizeof(ota.msg));
  xSemaphoreGive(mux);
}

static String releaseUrl(const String &repo, const char *file) {
  return "https://github.com/" + repo + "/releases/latest/download/" + file;
}

// Порівняння версій виду 2.3.10: >0, якщо a новіша за b
static int cmpVersion(const char *a, const char *b) {
  for (int i = 0; i < 3; i++) {
    char *ea, *eb;
    long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
    if (x != y) return x < y ? -1 : 1;
    a = (*ea == '.') ? ea + 1 : ea;
    b = (*eb == '.') ? eb + 1 : eb;
  }
  return 0;
}

// Повертає true, якщо в релізі новіша версія
static bool otaCheck(const String &repo) {
  if (repo.isEmpty()) { setOta(OtaSt::Error, "не вказано репозиторій"); return false; }
  setOta(OtaSt::Checking);

  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);   // GitHub перенаправляє на сховище файлів
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  int code = -1;
  String body;
  if (http.begin(c, releaseUrl(repo, "version.json"))) {
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
  }
  if (code != 200) {
    char m[72];
    snprintf(m, sizeof(m), code == 404 ? "реліз або version.json не знайдено" : "version.json: HTTP %d", code);
    setOta(OtaSt::Error, m);
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, body) || !d["version"].is<const char *>()) {
    setOta(OtaSt::Error, "version.json: невірний формат");
    return false;
  }
  String v = d["version"].as<String>();
  if (v.startsWith("v")) v.remove(0, 1);
  bool newer = cmpVersion(v.c_str(), FW_VERSION) > 0;

  xSemaphoreTake(mux, portMAX_DELAY);
  strlcpy(ota.latest, v.c_str(), sizeof(ota.latest));
  ota.st = newer ? OtaSt::Available : OtaSt::UpToDate;
  ota.msg[0] = 0;
  xSemaphoreGive(mux);
  Serial.printf("[ota] встановлено %s, у релізі %s\n", FW_VERSION, v.c_str());
  return newer;
}

static void otaInstall(const String &repo) {
  if (repo.isEmpty()) { setOta(OtaSt::Error, "не вказано репозиторій"); return; }
  setOta(OtaSt::Installing);
  otaBusy = true;                                // ledTask гасить стрічку
  vTaskDelay(pdMS_TO_TICKS(100));

  // 1) веб-інтерфейс у тимчасовий файл; підміняємо лише після успішної прошивки
  bool pageOk = false;
  {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    if (http.begin(c, releaseUrl(repo, "index.html"))) {
      if (http.GET() == 200) {
        File f = LittleFS.open("/index.new", "w");
        if (f) {
          int n = http.writeToStream(&f);
          f.close();
          pageOk = n > 0;
        }
      }
      http.end();
    }
    if (!pageOk) LittleFS.remove("/index.new");
  }

  // 2) прошивка
  WiFiClientSecure c;
  c.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return r = httpUpdate.update(c, releaseUrl(repo, OTA_FW_FILE));
  if (r == HTTP_UPDATE_OK) {
    if (pageOk) {
      LittleFS.remove("/index.html");
      LittleFS.rename("/index.new", "/index.html");
    }
    setOta(OtaSt::Installing, "готово, перезавантаження");
    Serial.println("[ota] оновлено, перезавантаження");
    delay(1500);
    ESP.restart();
  }
  LittleFS.remove("/index.new");
  setOta(OtaSt::Error, httpUpdate.getLastErrorString().c_str());
  Serial.printf("[ota] помилка: %s\n", httpUpdate.getLastErrorString().c_str());
  otaBusy = false;
}

void netTask(void *) {
  WiFiClientSecure client;
  client.setInsecure();        // без прив'язки до сертифіката — не зламається при його зміні
  String lastMod;
  uint32_t nextAt = 0;
  uint32_t nextOtaAt = millis() + 120000;        // перша автоперевірка через 2 хв після старту

  for (;;) {
    // оновлення з GitHub: за кнопкою або за розкладом
    if (!otaBusy && WiFi.status() == WL_CONNECTED) {
      bool autoDue = (int32_t)(millis() - nextOtaAt) >= 0;
      if (otaCheckReq || otaInstallReq || autoDue) {
        xSemaphoreTake(mux, portMAX_DELAY);
        String repo = cfg.otaRepo;
        bool autoOn = cfg.otaAuto;
        xSemaphoreGive(mux);
        bool install = otaInstallReq;
        bool check = otaCheckReq || (autoDue && repo.length());
        otaCheckReq = false;
        otaInstallReq = false;
        if (autoDue) nextOtaAt = millis() + OTA_CHECK_HOURS * 3600000UL;
        if (install) otaInstall(repo);
        else if (check && otaCheck(repo) && autoDue && autoOn) otaInstall(repo);
      }
    }

    if (!otaBusy && WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextAt) >= 0) {
      String token;
      uint8_t api;
      uint16_t interval;
      bool hadData;
      xSemaphoreTake(mux, portMAX_DELAY);
      token = cfg.token; api = cfg.apiVer; interval = cfg.intervalS; hadData = net.hasData;
      xSemaphoreGive(mux);

      nextAt = millis() + interval * 1000UL;
      if (token.isEmpty()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

      String url = String("https://") + API_HOST + (api == 2 ? "/v2" : "/v1") +
                   "/iot/active_air_raid_alerts.json";    // усі області, райони й громади одним рядком

      HTTPClient http;
      http.setConnectTimeout(8000);
      http.setTimeout(8000);
      int code = -1;
      String body;
      if (http.begin(client, url)) {
        http.addHeader("Authorization", "Bearer " + token);
        if (hadData && lastMod.length()) http.addHeader("If-Modified-Since", lastMod);
        const char *keys[] = {"Last-Modified"};
        http.collectHeaders(keys, 1);            // порівняння назв без урахування регістру
        code = http.GET();
        if (code == 200) {
          body = http.getString();               // коректно читає і chunked-відповіді
          String lm = http.header("Last-Modified");
          if (lm.length()) lastMod = lm;
        }
        http.end();
      }

      // Розбір: рядок у лапках, номер символу = UID. Пробіл означає «немає такої локації».
      static char buf[ALERT_BUF];
      uint16_t n = 0;
      bool parsed = false;
      if (code == 200) {
        int from = body.indexOf('"'), to = body.lastIndexOf('"');
        if (from >= 0 && to > from) {
          n = min((int)(ALERT_BUF - 1), to - from - 1);
          memcpy(buf, body.c_str() + from + 1, n);
          buf[n] = 0;
          // перевірка: позиції областей 3..31 мають містити лише відомі символи
          parsed = n >= 32;
          for (int u = 3; parsed && u <= 31; u++) parsed = strchr("ANPRY ", buf[u]) != nullptr;
        }
        if (!parsed) code = -100;                // 200, але тіло не те, що очікували
      }

      xSemaphoreTake(mux, portMAX_DELAY);
      net.http = code;
      if (parsed) {
        memcpy(net.alerts, buf, n + 1);
        net.len = n;
        net.hasData = true;
        net.lastOkMs = millis();
        net.okCnt++;
      } else if (code == 304 && net.hasData) {
        net.lastOkMs = millis();
        net.okCnt++;
      } else {
        net.errCnt++;
      }
      xSemaphoreGive(mux);

      if (code != 200 && code != 304) lastMod = "";            // після помилки просимо повне тіло
      if (code == 429) nextAt = millis() + 60000;              // перевищили ліміт — пауза хвилину

      Serial.printf("[net] HTTP %d, символів: %u\n", code, (unsigned)(parsed ? n : 0));
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ======================= Світлодіоди (ядро 1) =======================

Adafruit_NeoPixel strip(DEFAULT_LED_COUNT, DEFAULT_LED_PIN, NEO_GRB + NEO_KHZ800);

enum class Mode : uint8_t { Normal, Identify, Highlight, Test };
volatile Mode     mode = Mode::Normal;
volatile int      identLed = -1;
volatile int      hlUid = -1;
volatile char     testCh = 'N';         // перегляд: N, R, Y, P, M — усе разом, E — немає даних
volatile uint32_t modeUntil = 0;
volatile bool     stripDirty = false;   // веб-обробник просить ledTask перебудувати стрічку
volatile bool     apActive = false;

static inline uint32_t scaleColor(uint32_t c, uint8_t k) {
  uint8_t r = ((c >> 16) & 0xFF) * k / 255;
  uint8_t g = ((c >> 8) & 0xFF) * k / 255;
  uint8_t b = (c & 0xFF) * k / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Плавна хвиля lo..hi; квадрат робить зміну яскравості рівномірною для ока
static inline uint8_t wave(uint32_t periodMs, uint8_t lo, uint8_t hi) {
  float t = (millis() % periodMs) / (float)periodMs;
  float s = 0.5f - 0.5f * cosf(t * 2.0f * PI);
  s *= s;
  return lo + (uint8_t)((hi - lo) * s);
}

// Та сама хвиля, але від довільного часу t (для зсуву фази і зміни швидкості)
static inline uint8_t waveT(uint32_t t, uint32_t periodMs, uint8_t lo, uint8_t hi) {
  float x = (t % periodMs) / (float)periodMs;
  float s = 0.5f - 0.5f * cosf(x * 2.0f * PI);
  s *= s;
  return lo + (uint8_t)((hi - lo) * s);
}

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dUL; x ^= x >> 15; x *= 0x846ca68bUL; x ^= x >> 16;
  return x;
}

// Трикутник 0..255..0 за період
static inline uint8_t tri(uint32_t t, uint32_t periodMs) {
  uint32_t p = t % periodMs, half = periodMs / 2;
  return (p < half ? p : periodMs - p) * 255UL / half;
}

#define ALERT_ANIM_COUNT 12
#define CALM_ANIM_COUNT  9

// Яскравість тривожного світлодіода 0..255. i — номер світлодіода, t — час з урахуванням швидкості
static uint8_t alertLevel(uint8_t anim, uint16_t i, uint32_t t) {
  switch (anim) {
    default:
    case 0: return waveT(t, 1600, 40, 255);                       // пульсація
    case 1: return 255;                                           // рівне світіння
    case 2: return (t % 1000) < 500 ? 255 : 20;                   // блимання
    case 3: {                                                     // серцебиття: два удари і пауза
      uint32_t p = t % 1500;
      if (p < 300) return 35 + (uint16_t)tri(p, 300) * 220 / 255;
      if (p >= 380 && p < 760) return 35 + (uint16_t)tri(p - 380, 380) * 220 / 255;
      return 35;
    }
    case 4: return waveT(t + i * 140UL, 1600, 30, 255);           // хвиля, що біжить по світлодіодах
    case 5: return 110 + hash32(i * 7919UL + t / 70) % 146;       // мерехтіння, як полум'я
    case 6: return waveT(t, 4200, 12, 255);                       // повільне дихання
    case 7: {                                                     // стробоскоп: три короткі спалахи і пауза
      uint32_t p = t % 1400;
      return (p < 480 && (p / 80) % 2 == 0) ? 255 : 15;
    }
    // --- анімації з урахуванням розташування областей на карті ---
    case 8:  return waveT(t + (255 - ledX[i]) * 6UL, 1600, 30, 255);    // хвиля із заходу на схід
    case 9:  return waveT(t + (255 - ledY[i]) * 6UL, 1600, 30, 255);    // хвиля з півночі на південь
    case 10: return waveT(t + (255 - ledDist[i]) * 7UL, 1800, 30, 255); // кола від центру країни
    case 11: {                                                          // радар: промінь обертається навколо центру
      const uint8_t rot = (t % 3200) * 256UL / 3200;
      const uint8_t age = rot - ledAng[i];
      return age < 110 ? 255 - age * 2 : 35;
    }
  }
}

// Множник яскравості спокійного світлодіода 0..255
static uint8_t calmFactor(uint8_t anim, uint16_t i, uint32_t t) {
  switch (anim) {
    default:
    case 0: return 255;                                           // рівне світіння
    case 1: return waveT(t, 5000, 90, 255);                       // дихання всієї карти
    case 2: return waveT(t + i * 230UL, 3200, 70, 255);           // хвиля по світлодіодах
    case 3: {                                                     // зорі: окремі світлодіоди спалахують яскравіше
      uint32_t slot = t / 1300, f = t % 1300;
      return hash32(i * 7919UL + slot) % 5 == 0 ? 130 + (uint16_t)tri(f, 1300) * 125 / 255 : 130;
    }
    case 4: return 255 - waveT(t, 1600, 0, 190);                  // протифаза до тривоги, як у старій прошивці
    case 5: return 150 + hash32(i * 104729UL + t / 110) % 106;    // легке мерехтіння
    // --- з урахуванням розташування областей на карті ---
    case 6: return waveT(t + (255 - ledX[i]) * 12UL, 4000, 70, 255);     // повільна хвиля із заходу на схід
    case 7: return waveT(t + (255 - ledDist[i]) * 12UL, 4000, 70, 255);  // кола від центру країни
    case 8: {                                                            // радар
      const uint8_t rot = (t % 6000) * 256UL / 6000;
      const uint8_t age = rot - ledAng[i];
      return age < 120 ? 255 - age : 135;
    }
  }
}

static bool timeOk() { return time(nullptr) > 1700000000; }

uint8_t currentBrightness() {
  if (!cfg.nightOn || !timeOk()) return cfg.bright;
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  int h = tm.tm_hour;
  bool night = (cfg.nightFrom <= cfg.nightTo)
                 ? (h >= cfg.nightFrom && h < cfg.nightTo)
                 : (h >= cfg.nightFrom || h < cfg.nightTo);
  return night ? cfg.nightBright : cfg.bright;
}

// Викликається лише з setup() (до старту ledTask) та з самого ledTask
void applyStripConfig() {
  strip.clear();
  strip.show();
  strip.updateLength(cfg.ledCount);
  strip.setPin(cfg.ledPin);
  strip.begin();
  strip.clear();
  strip.show();
}

void render() {
  Mode m = mode;
  if (m != Mode::Normal && (int32_t)(millis() - modeUntil) >= 0) { mode = Mode::Normal; m = Mode::Normal; }

  static char a[ALERT_BUF];                      // static: щоб не займати стек задачі
  uint16_t len;
  bool hasData, stale;
  int lastHttp;
  xSemaphoreTake(mux, portMAX_DELAY);
  len = net.len;
  memcpy(a, net.alerts, len + 1);
  hasData = net.hasData;
  lastHttp = net.http;
  stale = isStale(net.hasData, net.lastOkMs, cfg.intervalS);
  xSemaphoreGive(mux);

  bool connected = WiFi.status() == WL_CONNECTED;

  // перегляд з веб-інтерфейсу: показуємо вибраний стан замість живих даних
  const bool test = (m == Mode::Test);
  const char tch = testCh;
  if (test) { hasData = true; stale = (tch == 'E'); }

  static const uint16_t SPEED_PCT[5] = {50, 75, 100, 150, 220};
  const uint32_t now = millis();
  const uint32_t t = (uint64_t)now * SPEED_PCT[constrain(cfg.animSpeed, 1, 5) - 1] / 100;
  static char lastCh[MAX_LEDS];                  // попередній стан кожного світлодіода
  static uint32_t changeAt[MAX_LEDS];
  static bool changeUp[MAX_LEDS];

  uint8_t br = currentBrightness();
  if ((m == Mode::Identify || m == Mode::Highlight) && br < 60) br = 60;   // під час прив'язки має бути добре видно
  strip.setBrightness(br);

  const uint8_t slow  = wave(3000, 15, 140);     // службові стани — повільне дихання
  const uint8_t blink = wave(600, 0, 255);
  const uint8_t calm  = (uint16_t)cfg.calmLevel * 255 / 100;
  const uint16_t count = strip.numPixels();

  for (uint16_t i = 0; i < count; i++) {
    const uint16_t uid = cfg.ledUid[i];
    const uint8_t parent = ledParent[i];
    uint32_t c = 0;

    if (m == Mode::Identify) {
      // поточний світлодіод — білий, уже прив'язані — ледь видно
      if ((int)i == identLed) c = 0xFFFFFF;
      else if (uid)           c = 0x060606;
    } else if (m == Mode::Highlight) {
      if (uid && (uid == hlUid || parent == hlUid)) c = scaleColor(0xFFFFFF, blink);
    } else if (!uid) {
      c = 0;
    } else if (!hasData && (!connected || lastHttp == 0)) {
      // ще не було жодної відповіді: точка доступу — бірюзове дихання, підключення — синє
      c = apActive ? scaleColor(0x00C8B4, slow) : scaleColor(0x0030FF, slow);
    } else if (stale) {
      c = scaleColor(cfg.cError, slow);          // даних немає — НЕ показуємо «все спокійно»
    } else {
      char ch;
      if (test) {
        ch = (tch == 'M') ? "NNNRYP"[hash32(uid) % 6] : tch;
      } else {
        ch = uid < len ? a[uid] : ' ';
        // тривога на всю область поширюється і на її райони
        if (parent && parent < len && (ch == 'N' || ch == ' ')) {
          char p = a[parent];
          if (p == 'A' || p == 'R' || p == 'Y') ch = p;
        }
      }
      const uint8_t lvl = alertLevel(cfg.alertAnim, i, t);
      switch (ch) {
        case 'A':
        case 'R': c = scaleColor(cfg.cRed, lvl); break;
        case 'Y': c = scaleColor(cfg.cYellow, lvl); break;
        case 'P': c = scaleColor(cfg.cPartial, lvl); break;
        case 'N': {
          const uint8_t ob = parent ? parent : (uint8_t)(uid < 32 ? uid : 0);
          const bool blue = (FLAG_BLUE_MASK >> ob) & 1;
          const uint8_t k = (uint16_t)calm * calmFactor(cfg.calmAnim, i, t) / 255;
          if (cfg.calmMode == 0)      c = scaleColor(blue ? cfg.cFlagBlue : cfg.cFlagYellow, k);
          else if (cfg.calmMode == 1) c = scaleColor(cfg.cCalm, k);
          break;
        }
        default: c = 0x181818; break;            // для цього UID немає даних — тьмяно-білий
      }

      // спалах при зміні стану: початок тривоги — білий строб, відбій — зелене блимання
      if (!test) {
        const bool wasAlert = lastCh[i] == 'A' || lastCh[i] == 'R' || lastCh[i] == 'Y';
        const bool isAlert = ch == 'A' || ch == 'R' || ch == 'Y';
        if (lastCh[i] && wasAlert != isAlert && (ch == 'N' || isAlert)) {
          changeAt[i] = now ? now : 1;
          changeUp[i] = isAlert;
        }
        lastCh[i] = ch;
        if (cfg.changeFx && changeAt[i] && now - changeAt[i] < 3000) {
          if (changeUp[i]) { if ((now / 120) % 2 == 0) c = 0xFFFFFF; }
          else c = scaleColor(0x00FF50, tri(now, 600));
        }
      }
    }
    strip.setPixelColor(i, c);
  }
  strip.show();
}

volatile uint32_t ledFrames = 0;                 // лічильник кадрів, для діагностики
TaskHandle_t ledHandle = nullptr;

void ledTask(void *) {
  TickType_t last = xTaskGetTickCount();
  bool blanked = false;
  for (;;) {
    if (stripDirty) { stripDirty = false; applyStripConfig(); }
    if (otaBusy) {
      if (!blanked) { strip.clear(); strip.show(); blanked = true; }
    } else {
      blanked = false;
      render();
    }
    ledFrames++;
    vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
  }
}

// ======================= Wi-Fi =======================

WebServer server(80);
DNSServer dns;
char devName[24];
uint32_t staConnectedSince = 0;
uint32_t staLostSince = 0;

void startAP() {
  if (apActive) return;
  WiFi.mode(cfg.ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(devName, AP_PASSWORD);
  delay(100);
#if defined(LOW_TX_POWER) && LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);            // те саме обмеження і для точки доступу
#endif
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  Serial.printf("[wifi] точка доступу %s, пароль %s, http://%s\n", devName, AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());
}

void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("[wifi] точку доступу вимкнено");
}

void wifiWatch() {
  static bool wasConnected = false;
  bool now = WiFi.status() == WL_CONNECTED;

  if (now && !wasConnected) {
    staConnectedSince = millis();
    Serial.printf("[wifi] підключено, IP %s, RSSI %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTzTime(TZ_KYIV, "pool.ntp.org", "time.google.com");
  }
  if (!now && wasConnected) staLostSince = millis();
  wasConnected = now;

  // Після хвилини стабільного зв'язку точка доступу більше не потрібна
  if (now && apActive && millis() - staConnectedSince > 60000) stopAP();

  // Зв'язку немає довше 2 хв — піднімаємо точку доступу, підключення триває у фоні
  if (!now && !apActive && staLostSince && millis() - staLostSince > 120000) startAP();
}

// ======================= Веб-сервер =======================

void sendJson(JsonDocument &doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }

bool readJson(JsonDocument &doc) {
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return false; }
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return false; }
  return true;
}

static String hex6(uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(b);
}

static uint32_t parseHex(const char *s, uint32_t def) {
  if (!s) return def;
  if (*s == '#') s++;
  return strlen(s) == 6 ? strtoul(s, nullptr, 16) : def;
}

void handleState() {
  JsonDocument d;
  xSemaphoreTake(mux, portMAX_DELAY);
  d["alerts"] = net.hasData ? String(net.alerts) : String("");
  d["http"]   = net.http;
  d["age"]    = net.hasData ? (int32_t)((millis() - net.lastOkMs) / 1000) : -1;
  d["stale"]  = isStale(net.hasData, net.lastOkMs, cfg.intervalS);
  d["ok"]     = net.okCnt;
  static const char *const OTA_NAMES[] = {"idle", "checking", "uptodate", "available", "installing", "error"};
  d["ota"]["st"]     = OTA_NAMES[(uint8_t)ota.st];
  d["ota"]["latest"] = String(ota.latest);
  d["ota"]["msg"]    = String(ota.msg);
  d["err"]    = net.errCnt;
  xSemaphoreGive(mux);
  bool up = WiFi.status() == WL_CONNECTED;
  d["wifi"]   = up;
  d["ssid"]   = up ? WiFi.SSID() : String("");
  d["rssi"]   = up ? WiFi.RSSI() : 0;
  d["ip"]     = WiFi.localIP().toString();
  d["ap"]     = (bool)apActive;
  d["uptime"] = millis() / 1000;
  d["heap"]   = ESP.getFreeHeap();
  d["chip"]   = String(ESP.getChipModel()) + ", ядер: " + String(ESP.getChipCores());
  d["ver"]    = FW_VERSION;
  d["timeOk"] = timeOk();
  d["bright"] = currentBrightness();
  d["test"]   = mode == Mode::Test;
  // діагностика світлодіодів
  d["led"]["pin"]    = cfg.ledPin;
  d["led"]["count"]  = strip.numPixels();
  d["led"]["frames"] = (uint32_t)ledFrames;
  d["led"]["busy"]   = (bool)otaBusy;
  d["led"]["stack"]  = ledHandle ? (uint32_t)uxTaskGetStackHighWaterMark(ledHandle) : 0;
  d["reset"]  = (int)esp_reset_reason();
  d["build"]  = String(__DATE__ " " __TIME__) + ", SDK " + ESP.getSdkVersion();
  sendJson(d);
}

void handleGetConfig() {
  JsonDocument d;
  xSemaphoreTake(mux, portMAX_DELAY);
  String t = cfg.token;
  d["otaRepo"] = cfg.otaRepo;
  xSemaphoreGive(mux);
  d["otaAuto"] = cfg.otaAuto;
  // токен повністю не віддаємо — тільки останні 4 символи
  d["tokenSet"]    = t.length() > 0;
  d["tokenTail"]   = t.length() > 4 ? t.substring(t.length() - 4) : String("");
  d["api"]         = cfg.apiVer;
  d["interval"]    = cfg.intervalS;
  d["pin"]         = cfg.ledPin;
  d["bright"]      = cfg.bright;
  d["nightOn"]     = cfg.nightOn;
  d["nightBright"] = cfg.nightBright;
  d["nightFrom"]   = cfg.nightFrom;
  d["nightTo"]     = cfg.nightTo;
  d["calmMode"]    = cfg.calmMode;
  d["calmLevel"]   = cfg.calmLevel;
  d["alertAnim"]   = cfg.alertAnim;
  d["calmAnim"]    = cfg.calmAnim;
  d["animSpeed"]   = cfg.animSpeed;
  d["changeFx"]    = cfg.changeFx;
  d["cCalm"]       = hex6(cfg.cCalm);
  d["cRed"]        = hex6(cfg.cRed);
  d["cYellow"]     = hex6(cfg.cYellow);
  d["cPartial"]    = hex6(cfg.cPartial);
  d["cError"]      = hex6(cfg.cError);
  d["cFlagBlue"]   = hex6(cfg.cFlagBlue);
  d["cFlagYellow"] = hex6(cfg.cFlagYellow);
  d["ssid"]        = cfg.ssid;
  JsonArray m = d["map"].to<JsonArray>();
  for (uint16_t i = 0; i < cfg.ledCount; i++) m.add(cfg.ledUid[i]);
  sendJson(d);
}

void handleSetConfig() {
  JsonDocument d;
  if (!readJson(d)) return;

  if (d["pin"].is<int>() && !pinOk(d["pin"].as<int>())) {
    server.send(400, "application/json", "{\"error\":\"pin\"}");
    return;
  }

  // репозиторій: приймаємо і повне посилання, і "користувач/репозиторій"; порожній рядок вимикає
  String repo;
  bool repoSet = d["otaRepo"].is<const char *>();
  if (repoSet) {
    repo = d["otaRepo"].as<String>();
    repo.trim();
    repo.replace("https://github.com/", "");
    repo.replace("http://github.com/", "");
    if (repo.endsWith("/")) repo.remove(repo.length() - 1);
    if (repo.endsWith(".git")) repo.remove(repo.length() - 4);
    bool ok = repo.length() <= 100;
    int slashes = 0;
    for (size_t i = 0; ok && i < repo.length(); i++) {
      char ch = repo[i];
      if (ch == '/') slashes++;
      else ok = isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.';
    }
    if (repo.length() && (!ok || slashes != 1 || repo.startsWith("/") || repo.endsWith("/"))) {
      server.send(400, "application/json", "{\"error\":\"repo\"}");
      return;
    }
  }

  xSemaphoreTake(mux, portMAX_DELAY);
  if (repoSet) cfg.otaRepo = repo;
  if (d["otaAuto"].is<bool>()) cfg.otaAuto = d["otaAuto"].as<bool>();
  if (d["token"].is<const char *>()) {
    String t = stripBearer(d["token"].as<String>());
    if (t.length()) cfg.token = t;               // порожнє поле — токен не міняємо
  }
  if (d["api"].is<int>())      cfg.apiVer    = d["api"].as<int>() == 1 ? 1 : 2;
  if (d["interval"].is<int>()) cfg.intervalS = constrain(d["interval"].as<int>(), 15, 120);
  xSemaphoreGive(mux);

  if (d["pin"].is<int>()) {
    uint8_t p = d["pin"].as<int>();
    if (p != cfg.ledPin) { cfg.ledPin = p; stripDirty = true; }
  }
  if (d["bright"].is<int>())      cfg.bright      = constrain(d["bright"].as<int>(), 1, 255);
  if (d["nightOn"].is<bool>())    cfg.nightOn     = d["nightOn"].as<bool>();
  if (d["nightBright"].is<int>()) cfg.nightBright = constrain(d["nightBright"].as<int>(), 0, 255);
  if (d["nightFrom"].is<int>())   cfg.nightFrom   = constrain(d["nightFrom"].as<int>(), 0, 23);
  if (d["nightTo"].is<int>())     cfg.nightTo     = constrain(d["nightTo"].as<int>(), 0, 23);
  if (d["calmMode"].is<int>())    cfg.calmMode    = constrain(d["calmMode"].as<int>(), 0, 2);
  if (d["calmLevel"].is<int>())   cfg.calmLevel   = constrain(d["calmLevel"].as<int>(), 0, 100);
  if (d["alertAnim"].is<int>())   cfg.alertAnim   = constrain(d["alertAnim"].as<int>(), 0, ALERT_ANIM_COUNT - 1);
  if (d["calmAnim"].is<int>())    cfg.calmAnim    = constrain(d["calmAnim"].as<int>(), 0, CALM_ANIM_COUNT - 1);
  if (d["animSpeed"].is<int>())   cfg.animSpeed   = constrain(d["animSpeed"].as<int>(), 1, 5);
  if (d["changeFx"].is<bool>())   cfg.changeFx    = d["changeFx"].as<bool>();
  cfg.cCalm    = parseHex(d["cCalm"].as<const char *>(),    cfg.cCalm);
  cfg.cRed     = parseHex(d["cRed"].as<const char *>(),     cfg.cRed);
  cfg.cYellow  = parseHex(d["cYellow"].as<const char *>(),  cfg.cYellow);
  cfg.cPartial = parseHex(d["cPartial"].as<const char *>(), cfg.cPartial);
  cfg.cError   = parseHex(d["cError"].as<const char *>(),   cfg.cError);
  cfg.cFlagBlue   = parseHex(d["cFlagBlue"].as<const char *>(),   cfg.cFlagBlue);
  cfg.cFlagYellow = parseHex(d["cFlagYellow"].as<const char *>(), cfg.cFlagYellow);

  // ?save=0 — застосувати лише до наступного перезавантаження (для перегляду анімацій)
  if (server.arg("save") != "0") saveSettings();
  sendOk();
}

// Перегляд на карті: {"state":"N"|"R"|"Y"|"P"|"M"|"E"}, порожній рядок — назад до живих даних
void handleTest() {
  JsonDocument d;
  if (!readJson(d)) return;
  String st = d["state"] | "";
  if (st.length() == 1 && strchr("NRYPME", st[0])) {
    testCh = st[0];
    modeUntil = millis() + 60000;                // через хвилину карта сама повертається до живих даних
    mode = Mode::Test;
  } else {
    mode = Mode::Normal;
  }
  sendOk();
}

void handleSetMap() {
  JsonDocument d;
  if (!readJson(d)) return;
  JsonArray m = d["map"].as<JsonArray>();
  if (m.isNull() || m.size() == 0) { server.send(400, "application/json", "{\"error\":\"no map\"}"); return; }
  uint16_t n = m.size() > MAX_LEDS ? MAX_LEDS : m.size();
  for (uint16_t i = 0; i < MAX_LEDS; i++) {
    int u = (i < n) ? (m[i] | 0) : 0;
    cfg.ledUid[i] = (u > 0 && u < ALERT_BUF) ? u : 0;
  }
  rebuildParents();
  if (n != cfg.ledCount) { cfg.ledCount = n; stripDirty = true; }
  saveMap();
  sendOk();
}

void handleIdentify() {
  JsonDocument d;
  if (!readJson(d)) return;
  int led = d["led"] | -1;
  // під час майстра кількість світлодіодів може бути більшою за збережену
  int count = d["count"] | (int)cfg.ledCount;
  if (count >= 1 && count <= MAX_LEDS && count != cfg.ledCount) {
    cfg.ledCount = count;
    stripDirty = true;
  }
  if (led < 0 || led >= cfg.ledCount) {
    mode = Mode::Normal;
  } else {
    identLed = led;
    modeUntil = millis() + 120000;               // сам повернеться в робочий режим через 2 хв
    mode = Mode::Identify;
  }
  sendOk();
}

void handleHighlight() {
  JsonDocument d;
  if (!readJson(d)) return;
  int u = d["uid"] | 0;
  if (u > 0 && u < ALERT_BUF) {
    hlUid = u;
    modeUntil = millis() + 4000;
    mode = Mode::Highlight;
  } else {
    mode = Mode::Normal;
  }
  sendOk();
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); n = WIFI_SCAN_RUNNING; }
  JsonDocument d;
  if (n == WIFI_SCAN_RUNNING) { d["scanning"] = true; sendJson(d); return; }
  d["scanning"] = false;
  JsonArray arr = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : arr) {
      if (o["ssid"].as<String>() == s) { dup = true; break; }
    }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

void handleWifi() {
  JsonDocument d;
  if (!readJson(d)) return;
  String s = d["ssid"] | "";
  String p = d["pass"] | "";
  s.trim();
  if (s.isEmpty() || s.length() > 32 || p.length() > 64) {
    server.send(400, "application/json", "{\"error\":\"ssid\"}");
    return;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  cfg.ssid = s;
  cfg.pass = p;
  xSemaphoreGive(mux);
  saveWifi();
  sendOk();
  delay(800);
  ESP.restart();
}

void handleRoot() {
  File f = LittleFS.open("/index.html", "r");
  if (f) { server.streamFile(f, "text/html"); f.close(); return; }
  // запасна сторінка, якщо LittleFS порожня
  server.send(200, "text/html",
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>AlertMap</title><body style='font-family:sans-serif;padding:16px;max-width:520px'>"
    "<h3>Веб-інтерфейс не знайдено</h3><p>Залий файлову систему (Upload Filesystem Image) або завантаж index.html тут:</p>"
    "<form method=POST action=/fs enctype=multipart/form-data><input type=file name=f> <button>Завантажити</button></form>"
    "<h3>Прошивка (.bin)</h3><form method=POST action=/update enctype=multipart/form-data><input type=file name=f> <button>Оновити</button></form>");
}

void setupServer() {
  static bool otaOk = false;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state",     HTTP_GET,  handleState);
  server.on("/api/config",    HTTP_GET,  handleGetConfig);
  server.on("/api/config",    HTTP_POST, handleSetConfig);
  server.on("/api/map",       HTTP_POST, handleSetMap);
  server.on("/api/identify",  HTTP_POST, handleIdentify);
  server.on("/api/highlight", HTTP_POST, handleHighlight);
  server.on("/api/test",      HTTP_POST, handleTest);
  server.on("/api/scan",      HTTP_GET,  handleScan);
  server.on("/api/wifi",      HTTP_POST, handleWifi);
  server.on("/api/ota/check",   HTTP_POST, []() { otaCheckReq = true; sendOk(); });
  server.on("/api/ota/install", HTTP_POST, []() { otaInstallReq = true; sendOk(); });
  server.on("/api/reboot",    HTTP_POST, []() {
    sendOk();
    delay(500);
    ESP.restart();
  });

  // Оновлення прошивки по Wi-Fi
  server.on("/update", HTTP_POST,
    []() {
      server.send(otaOk ? 200 : 500, "application/json", otaOk ? "{\"ok\":true}" : "{\"ok\":false}");
      if (otaOk) { delay(800); ESP.restart(); }
      otaBusy = false;
    },
    []() {
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        otaOk = false;
        otaBusy = true;                          // ledTask гасить стрічку, netTask чекає
        Serial.printf("[ota] %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_END) {
        otaOk = Update.end(true);
        if (otaOk) Serial.printf("[ota] готово, %u байт\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        otaBusy = false;
      }
    });

  // Завантаження окремого файлу в LittleFS (наприклад, нового index.html)
  server.on("/fs", HTTP_POST,
    []() { sendOk(); },
    []() {
      static File f;
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        String name = up.filename;
        if (!name.startsWith("/")) name = "/" + name;
        f = LittleFS.open(name, "w");
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (f) f.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (f) f.close();
      }
    });

  // Captive portal: у режимі точки доступу будь-яка адреса веде на налаштування
  server.onNotFound([]() {
    if (apActive && server.hostHeader() != WiFi.softAPIP().toString()) {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

// ======================= setup / loop =======================

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);                      // не гальмувати, коли USB-монітор не відкрито
#endif
  delay(300);
  mux = xSemaphoreCreateMutex();

  loadSettings();
  applyStripConfig();

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS не змонтовано");

  uint64_t mac = ESP.getEfuseMac();
  snprintf(devName, sizeof(devName), "AlertMap-%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));

  // Анімація стартує одразу, ще до підключення до мережі
  xTaskCreatePinnedToCore(ledTask, "led", 8192, nullptr, 2, &ledHandle, CORE_LED);

  WiFi.persistent(false);                        // облікові дані зберігаємо самі, в NVS
  WiFi.setHostname(devName);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
#if defined(LOW_TX_POWER) && LOW_TX_POWER
    WiFi.setTxPower(WIFI_POWER_8_5dBm);          // дешеві C3 SuperMini нестабільні на повній потужності
#endif
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) startAP();

  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  setupServer();
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, CORE_NET);
  Serial.printf("[sys] %s v%s готово, причина запуску %d, світлодіодів %u на GPIO %u\n", devName, FW_VERSION,
                (int)esp_reset_reason(), (unsigned)cfg.ledCount, (unsigned)cfg.ledPin);
}

void loop() {
  server.handleClient();
  if (apActive) dns.processNextRequest();
  wifiWatch();
  delay(2);
}

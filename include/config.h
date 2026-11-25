#pragma once
// ВЕРСІЯ 2.6.2 · оновлено 26.11.2026 02:00

// ---- Значення за замовчуванням (усе змінюється з веб-інтерфейсу) ----

#define FW_VERSION        "2.6.2"

// Токен alerts.in.ua вводиться у веб-інтерфейсі (Налаштування) і зберігається в пам'яті пристрою.
// Тут його краще не писати: якщо проєкт лежить у публічному репозиторії, токен побачать усі.
#define DEFAULT_TOKEN     ""

// Оновлення з GitHub Releases: репозиторій за замовчуванням ("користувач/репозиторій"),
// можна залишити порожнім і вказати у веб-інтерфейсі (Система).
#define DEFAULT_OTA_REPO  ""
#define OTA_CHECK_HOURS   24
#if CONFIG_IDF_TARGET_ESP32C3
  #define OTA_FW_FILE     "firmware-esp32c3.bin"
#else
  #define OTA_FW_FILE     "firmware-esp32s3.bin"
#endif

// Пін даних стрічки (змінюється у веб-інтерфейсі).
//   ESP32-S3: підходять 4–18, 21, 38–42, 47. 48 (або 38) — вбудований RGB на DevKitC-1.
//   ESP32-C3: підходять 0, 1, 3–7, 10.
#define DEFAULT_LED_PIN   4
#define DEFAULT_LED_COUNT 27
#define MAX_LEDS          300

// Рядок API: номер символу = UID локації (області 3..31, райони, громади до ~1911)
#define ALERT_BUF         2048

// Точка доступу для налаштування (коли Wi-Fi не задано або недоступний)
#define AP_PASSWORD       "alertmap"     // мінімум 8 символів
#define MDNS_NAME         "alertmap"     // http://alertmap.local

#define API_HOST          "api.alerts.in.ua"

// Часовий пояс Києва (для нічного режиму)
#define TZ_KYIV           "EET-2EEST,M3.5.0/3,M10.5.0/4"

// Скільки секунд без успішної відповіді вважати «немає даних»
#define STALE_MIN_SEC     180

// Частота оновлення світлодіодів, мс на кадр
#define FRAME_MS          25

// Кількість анімацій (див. alertLevel() і calmFactor() у main.cpp)
#define ALERT_ANIM_COUNT  12
#define CALM_ANIM_COUNT   9

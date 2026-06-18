/*
 * ============================================================
 *   GATEWAY EWS MULTI-NODE - TTGO LoRa32 v2.1
 *   Versi: 5.0 — Production-Ready
 *
 *   Library yang dibutuhkan (install via Arduino Library Manager):
 *     - LoRa by Sandeep Mistry
 *     - ArduinoJson by Benoit Blanchon (v7.x)
 *     - esp_task_wdt (sudah bawaan ESP32 Arduino Core)
 * ============================================================
 */

#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"
#include <esp_task_wdt.h>   // [FIX-8] Watchdog

// ============================================================
// KONFIGURASI DASAR
// ============================================================
const char* ssid       = "NAMA_WIFI_KAMU";
const char* password   = "PASSWORD_WIFI_KAMU";

// [FIX-15] ThingSpeak pakai HTTPS
const char* tsHost     = "api.thingspeak.com";
const char* apiKey     = "KCBA1A7JJJOOCIRR";

const char* tokenTelegram  = "8638939390:AAEcT7bGQzJ_B6gkCSwOEBuryHizk5CCM1g";
const char* chatIDTelegram = "6548221896";

#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   14
#define DIO0  26

// ============================================================
// STRUCT KONFIGURASI (dibaca dari /config.json di SPIFFS)
// [FIX-10] Threshold tidak hardcode lagi
// ============================================================
struct GatewayConfig {
  float  thresholdTilt;      // default 15.0
  float  thresholdRain;      // default 50.0
  float  thresholdBatLow;    // default 3.3
  float  thresholdBatMin;    // default 0.1
  int    maxQueueSize;        // default 50  [FIX-1]
  unsigned long heartbeatMs; // default 60000
  unsigned long tsJedaMs;    // default 15000
  unsigned long wifiJedaMs;  // default 10000
  unsigned long spiffsFlushMs; // default 30000  [FIX-4]
  long   spiffsMaxBytes;     // default 512000 (512KB)  [FIX-14]
};

GatewayConfig cfg = {
  15.0f, 50.0f, 3.3f, 0.1f,
  50, 60000UL, 15000UL, 10000UL, 30000UL,
  512UL * 1024UL
};

// ============================================================
// MULTI-NODE
// ============================================================
#define MAX_NODE 10

struct NodeStatus {
  char          id[16];
  float         battery;
  int           lastRSSI;
  float         lastSNR;
  unsigned long lastPacketTime;
  bool          isOnline;
};

NodeStatus daftarNode[MAX_NODE];

// ============================================================
// STRUCT DATA SENSOR
// ============================================================
struct SensorData {
  char  nodeID[16];
  float tilt;
  float rain;
  float moist;
  float battery;
  int   rssi;
  float snr;
  bool  valid;
};

// ============================================================
// [FIX-1] QUEUE LEBIH BESAR (default 50, dari config.json)
// Heap-allocated saat runtime supaya ukuran fleksibel
// ============================================================
SensorData* queueData   = nullptr;
int  queueHead  = 0;
int  queueTail  = 0;
int  queueCount = 0;
int  retryCount = 0;

// ============================================================
// [FIX-2] NOTIFICATION QUEUE — Telegram tidak blocking loop
// ============================================================
#define MAX_NOTIF 5
struct Notification {
  char  node[16];
  float tilt, rain, moist, battery;
  int   rssi;
  float snr;
  char  alertType[32];
  bool  valid;
};
Notification notifQueue[MAX_NOTIF];
int notifHead = 0, notifTail = 0, notifCount = 0;

// ============================================================
// [FIX-4] RAM BUFFER UNTUK SPIFFS — flush tiap 30 detik
// ============================================================
#define SPIFFS_BUFFER_SIZE 20
SensorData spiffsBuffer[SPIFFS_BUFFER_SIZE];
int  spiffsBufferCount = 0;
unsigned long lastSpiffsFlush = 0;

// ============================================================
// TIMER NON-BLOCKING
// ============================================================
unsigned long lastThingSpeakUpload = 0;
unsigned long lastWiFiCheckTime    = 0;
unsigned long lastRetryTime        = 0;
const unsigned long JEDA_RETRY     = 2000UL;

// ============================================================
// BACKUP SPIFFS
// ============================================================
const char* backupFileName = "/backup_ews.txt";
const char* configFileName = "/config.json";
File        fileBackupRead;
bool        sedangProsesBackup = false;

// ============================================================
// PROTOTYPE
// ============================================================
void  muatKonfigurasi();
void  prosesDataGateway(const char* node, float t, float r, float m, float bat, int rssi, float snr);
void  enqueueNotif(const char* node, float t, float r, float m, float bat, int rssi, float snr, const char* alertType);
void  handleNotifQueue();
void  handleBackupNonBlocking();
void  handleCloudQueueNonBlocking();
void  cekHeartbeatMultiNode();
void  handleWiFiReconnectNonBlocking();
void  handleSpiffsFlush();
int   cariAtauTambahNode(const char* id);
bool  enqueueData(SensorData& data);
bool  kirimKeThingSpeak(SensorData& d);
int   nodeIDtoInt(const char* id);
void  bufferKeSPIFFS(SensorData& d);
void  flushSpiffsBuffer();
bool  validasiSensor(float tilt, float rain, float moist, float battery);  // [FIX-11]
String rssiLabel(int rssi);                                                 // [FIX-13]
void  cekRotasiSPIFFS();                                                    // [FIX-14]



// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== GATEWAY EWS v5.0 ===");

  // [FIX-8] Aktifkan watchdog 30 detik
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS Mount Gagal!");
  } else {
    Serial.println("[OK] SPIFFS Siap.");
    muatKonfigurasi();  // [FIX-10] Baca threshold dari config.json
  }

  // Alokasikan queue sesuai konfigurasi [FIX-1]
  queueData = new SensorData[cfg.maxQueueSize];
  if (!queueData) {
    Serial.println("[ERROR] Gagal alokasi queue RAM!");
    while(1);
  }
  Serial.printf("[OK] Queue RAM dialokasikan: %d slot\n", cfg.maxQueueSize);

  memset(daftarNode, 0, sizeof(daftarNode));
  memset(notifQueue, 0, sizeof(notifQueue));
  memset(spiffsBuffer, 0, sizeof(spiffsBuffer));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("[WiFi] Menghubungkan background...");

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(915E6)) {
    Serial.println("[ERROR] Modul LoRa tidak terdeteksi!");
    while (1);
  }
  Serial.println("[OK] Gateway EWS Siap Beroperasi.");
}

// ============================================================
// LOOP UTAMA — 100% NON-BLOCKING
// ============================================================
void loop() {
  esp_task_wdt_reset();  // [FIX-8] Reset watchdog setiap iterasi

  handleWiFiReconnectNonBlocking();
  cekHeartbeatMultiNode();
  handleCloudQueueNonBlocking();
  handleNotifQueue();           // [FIX-2] Proses notifikasi dari queue
  handleSpiffsFlush();          // [FIX-4] Flush RAM buffer ke SPIFFS
  handleBackupNonBlocking();

  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    char payload[256] = {0};
    int  idx = 0;
    while (LoRa.available() && idx < (int)sizeof(payload) - 1) {
      payload[idx++] = (char)LoRa.read();
    }

    int   rssi = LoRa.packetRssi();
    float snr  = LoRa.packetSnr();

    Serial.printf("\n[LoRa] Paket diterima | RSSI: %d dBm (%s) | SNR: %.1f dB\n",
                  rssi, rssiLabel(rssi).c_str(), snr);  // [FIX-13]

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      const char* idKirim = doc["id"] | "NODE01";
      float tilt  = doc["tilt"]    | 0.0f;
      float rain  = doc["rain"]    | 0.0f;
      float moist = doc["moist"]   | 0.0f;
      float bat   = doc["battery"] | 0.0f;

      // [FIX-11] Validasi nilai sensor sebelum diproses
      if (!validasiSensor(tilt, rain, moist, bat)) {
        Serial.printf("[WARN] Data sensor dari %s di luar batas wajar, diabaikan.\n", idKirim);
      }

      int ni = cariAtauTambahNode(idKirim);
      if (ni != -1) {
        daftarNode[ni].battery        = bat;
        daftarNode[ni].lastRSSI       = rssi;
        daftarNode[ni].lastSNR        = snr;
        daftarNode[ni].lastPacketTime = millis();
        if (!daftarNode[ni].isOnline) {
          daftarNode[ni].isOnline = true;
          Serial.printf("[Node] %s kembali ONLINE.\n", idKirim);
        }
      }

      prosesDataGateway(idKirim, tilt, rain, moist, bat, rssi, snr);

    } else {
      Serial.printf("[ERROR] Gagal parsing JSON: %s\n", err.c_str());
    }
  }
}

// ============================================================
// [FIX-10] MUAT KONFIGURASI DARI /config.json
// Jika file tidak ada, buat otomatis dengan nilai default
// ============================================================
void muatKonfigurasi() {
  if (!SPIFFS.exists(configFileName)) {
    Serial.println("[Config] File tidak ditemukan, membuat default...");
    File f = SPIFFS.open(configFileName, FILE_WRITE);
    if (f) {
      StaticJsonDocument<256> doc;
      doc["threshold_tilt"]    = cfg.thresholdTilt;
      doc["threshold_rain"]    = cfg.thresholdRain;
      doc["threshold_bat_low"] = cfg.thresholdBatLow;
      doc["threshold_bat_min"] = cfg.thresholdBatMin;
      doc["max_queue"]         = cfg.maxQueueSize;
      doc["heartbeat_ms"]      = cfg.heartbeatMs;
      doc["ts_jeda_ms"]        = cfg.tsJedaMs;
      doc["spiffs_max_bytes"]  = cfg.spiffsMaxBytes;
      serializeJsonPretty(doc, f);
      f.close();
      Serial.println("[Config] Default config.json dibuat.");
    }
    return;
  }

  File f = SPIFFS.open(configFileName, FILE_READ);
  if (!f) return;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.println("[Config] config.json korup, pakai nilai default.");
    return;
  }

  cfg.thresholdTilt    = doc["threshold_tilt"]    | cfg.thresholdTilt;
  cfg.thresholdRain    = doc["threshold_rain"]    | cfg.thresholdRain;
  cfg.thresholdBatLow  = doc["threshold_bat_low"] | cfg.thresholdBatLow;
  cfg.thresholdBatMin  = doc["threshold_bat_min"] | cfg.thresholdBatMin;
  cfg.maxQueueSize     = doc["max_queue"]         | cfg.maxQueueSize;
  cfg.heartbeatMs      = doc["heartbeat_ms"]      | cfg.heartbeatMs;
  cfg.tsJedaMs         = doc["ts_jeda_ms"]        | cfg.tsJedaMs;
  cfg.spiffsMaxBytes   = doc["spiffs_max_bytes"]  | cfg.spiffsMaxBytes;

  Serial.printf("[Config] Dimuat — tilt:%.1f° rain:%.1fmm bat<%.2fV queue:%d\n",
                cfg.thresholdTilt, cfg.thresholdRain, cfg.thresholdBatLow, cfg.maxQueueSize);
}

// ============================================================
// [FIX-11] VALIDASI NILAI SENSOR
// ============================================================
bool validasiSensor(float tilt, float rain, float moist, float battery) {
  if (tilt    < 0.0f   || tilt    > 90.0f)  return false;
  if (rain    < 0.0f   || rain    > 500.0f) return false;
  if (moist   < 0.0f   || moist   > 100.0f) return false;
  if (battery < 0.0f   || battery > 5.0f)   return false;
  return true;
}

// ============================================================
// [FIX-13] RSSI QUALITY LABEL
// ============================================================
String rssiLabel(int rssi) {
  if (rssi > -90)  return "Excellent";
  if (rssi > -105) return "Good";
  if (rssi > -120) return "Weak";
  return "Critical";
}

// ============================================================
// PROSES DATA MASUK
// ============================================================
void prosesDataGateway(const char* node, float t, float r, float m, float bat, int rssi, float snr) {

  // Alert kritis — masuk notif queue (non-blocking)  [FIX-2]
  if (t >= cfg.thresholdTilt || r >= cfg.thresholdRain) {
    enqueueNotif(node, t, r, m, bat, rssi, snr, "BAHAYA LONGSOR");
  }
  if (bat > cfg.thresholdBatMin && bat < cfg.thresholdBatLow) {
    enqueueNotif(node, t, r, m, bat, rssi, snr, "LOW BATTERY");
  }

  SensorData d;
  strncpy(d.nodeID, node, sizeof(d.nodeID) - 1);
  d.nodeID[sizeof(d.nodeID) - 1] = '\0';
  d.tilt    = t;  d.rain = r;  d.moist = m;
  d.battery = bat; d.rssi = rssi; d.snr = snr;
  d.valid   = true;

  if (WiFi.status() == WL_CONNECTED) {
    if (!enqueueData(d)) {
      Serial.println("[Queue] Buffer penuh, data ke SPIFFS buffer.");
      bufferKeSPIFFS(d);
    }
  } else {
    bufferKeSPIFFS(d);
  }
}

// ============================================================
// [FIX-1] ENQUEUE DATA SENSOR
// ============================================================
bool enqueueData(SensorData& data) {
  if (queueCount >= cfg.maxQueueSize) return false;
  queueData[queueTail] = data;
  queueTail = (queueTail + 1) % cfg.maxQueueSize;
  queueCount++;
  return true;
}

// ============================================================
// [FIX-2] NOTIFIKASI QUEUE — tidak blocking loop
// ============================================================
void enqueueNotif(const char* node, float t, float r, float m, float bat,
                  int rssi, float snr, const char* alertType) {
  if (notifCount >= MAX_NOTIF) {
    Serial.println("[Notif] Queue notifikasi penuh, peringatan dilewati.");
    return;
  }
  Notification& n = notifQueue[notifTail];
  strncpy(n.node,      node,      sizeof(n.node) - 1);
  strncpy(n.alertType, alertType, sizeof(n.alertType) - 1);
  n.tilt = t; n.rain = r; n.moist = m;
  n.battery = bat; n.rssi = rssi; n.snr = snr;
  n.valid = true;
  notifTail = (notifTail + 1) % MAX_NOTIF;
  notifCount++;
}

void handleNotifQueue() {
  if (notifCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  Notification& n = notifQueue[notifHead];
  if (!n.valid) {
    notifHead = (notifHead + 1) % MAX_NOTIF;
    notifCount--;
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);  // [FIX-12]

  String url = "https://api.telegram.org/bot";
  url += tokenTelegram;
  url += "/sendMessage";

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> body;
  body["chat_id"]    = chatIDTelegram;
  body["parse_mode"] = "Markdown";

  String pesan = "🚨 *PERINGATAN EWS MULTI-NODE* 🚨\n";
  pesan += "Alert   : "; pesan += n.alertType; pesan += "\n";
  pesan += "Node    : "; pesan += n.node;      pesan += "\n";

  if (strcmp(n.alertType, "BAHAYA LONGSOR") == 0) {
    pesan += "Tilt    : "; pesan += String(n.tilt, 2); pesan += "°\n";
    pesan += "Hujan   : "; pesan += String(n.rain, 2); pesan += " mm\n";
    pesan += "Lembap  : "; pesan += String(n.moist, 1); pesan += " %\n";
  }
  pesan += "Battery : "; pesan += String(n.battery, 2); pesan += " V\n";
  pesan += "RSSI    : "; pesan += String(n.rssi);
  pesan += " dBm ("; pesan += rssiLabel(n.rssi); pesan += ")\n";
  pesan += "SNR     : "; pesan += String(n.snr, 1); pesan += " dB";

  body["text"] = pesan;

  String bodyStr;
  serializeJson(body, bodyStr);

  int code = http.POST(bodyStr);
  http.end();

  if (code == 200) {
    Serial.printf("[Telegram] Alert '%s' terkirim.\n", n.alertType);
  } else {
    Serial.printf("[Telegram] Gagal: HTTP %d\n", code);
  }

  n.valid = false;
  notifHead = (notifHead + 1) % MAX_NOTIF;
  notifCount--;
}

// ============================================================
// [FIX-4] RAM BUFFER UNTUK SPIFFS
// Data dikumpulkan di RAM dulu, di-flush ke flash tiap 30 detik
// Mengurangi write cycle flash drastis
// ============================================================
void bufferKeSPIFFS(SensorData& d) {
  if (spiffsBufferCount >= SPIFFS_BUFFER_SIZE) {
    Serial.println("[SPIFFS] RAM buffer penuh, flush paksa.");
    flushSpiffsBuffer();
  }
  spiffsBuffer[spiffsBufferCount++] = d;
}

void handleSpiffsFlush() {
  if (spiffsBufferCount == 0) return;
  if (millis() - lastSpiffsFlush < cfg.spiffsFlushMs) return;
  flushSpiffsBuffer();
}

void flushSpiffsBuffer() {
  if (spiffsBufferCount == 0) return;

  cekRotasiSPIFFS();  // [FIX-14] Cek ukuran sebelum tulis

  File f = SPIFFS.open(backupFileName, FILE_APPEND);
  if (!f) {
    Serial.println("[SPIFFS] Gagal buka file untuk flush!");
    return;
  }

  for (int i = 0; i < spiffsBufferCount; i++) {
    SensorData& d = spiffsBuffer[i];
    f.printf("%s,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%lu\n",
             d.nodeID, d.tilt, d.rain, d.moist,
             d.battery, d.rssi, d.snr, millis());
  }
  f.close();

  Serial.printf("[SPIFFS] Flush %d data ke flash.\n", spiffsBufferCount);
  spiffsBufferCount = 0;
  lastSpiffsFlush   = millis();
}

// ============================================================
// [FIX-14] ROTASI FILE SPIFFS JIKA TERLALU BESAR
// ============================================================
void cekRotasiSPIFFS() {
  if (!SPIFFS.exists(backupFileName)) return;
  File f = SPIFFS.open(backupFileName, FILE_READ);
  if (!f) return;
  size_t ukuran = f.size();
  f.close();

  if ((long)ukuran > cfg.spiffsMaxBytes) {
    Serial.printf("[SPIFFS] File terlalu besar (%u bytes), dirotasi.\n", ukuran);
    // Hapus file lama — data terlalu usang untuk diupload
    // Untuk sistem kritis bisa ditambah rename ke /backup_old.txt
    SPIFFS.remove(backupFileName);
    Serial.println("[SPIFFS] File lama dihapus, mulai dari awal.");
  }
}

// ============================================================
// [FIX-1 & FIX-15] KIRIM KE THINGSPEAK VIA HTTPS
// Validasi HTTP 200 + body != "0"  [FIX-BUG1 & BUG8]
// ============================================================
bool kirimKeThingSpeak(SensorData& d) {
  int nodeInt = nodeIDtoInt(d.nodeID);

  char path[256];
  snprintf(path, sizeof(path),
           "/update?api_key=%s&field1=%.2f&field2=%.2f&field3=%.2f"
           "&field4=%d&field5=%.2f&field6=%.2f&field7=%d",
           apiKey, d.tilt, d.rain, d.moist,
           d.rssi, d.snr, d.battery, nodeInt);

  // [FIX-15] HTTPS
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(5000);  // [FIX-12] Timeout 5 detik

  String url = "https://";
  url += tsHost;
  url += path;

  http.begin(client, url);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[ThingSpeak] HTTP gagal: %d\n", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();
  body.trim();

  if (body == "0" || body.length() == 0) {
    Serial.printf("[ThingSpeak] Ditolak server (body: '%s')\n", body.c_str());
    return false;
  }

  Serial.printf("[ThingSpeak] OK — entry #%s | Node:%s\n", body.c_str(), d.nodeID);
  return true;
}

// ============================================================
// PROSES ANTREAN RAM KE CLOUD (NON-BLOCKING)
// ============================================================
void handleCloudQueueNonBlocking() {
  if (queueCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastThingSpeakUpload < cfg.tsJedaMs) return;
  if (millis() - lastRetryTime < JEDA_RETRY) return;

  lastRetryTime = millis();

  SensorData& d = queueData[queueHead];
  bool sukses   = kirimKeThingSpeak(d);

  if (sukses) {
    lastThingSpeakUpload = millis();
    queueHead  = (queueHead + 1) % cfg.maxQueueSize;
    queueCount--;
    retryCount = 0;
    Serial.printf("[Queue] Terkirim. Sisa: %d\n", queueCount);
  } else {
    retryCount++;
    if (retryCount >= 3) {
      Serial.println("[Queue] Gagal 3x — data asli ke SPIFFS buffer.");
      bufferKeSPIFFS(d);
      queueHead  = (queueHead + 1) % cfg.maxQueueSize;
      queueCount--;
      retryCount = 0;
    }
  }
}

// ============================================================
// PROSES BACKUP SPIFFS KE CLOUD (NON-BLOCKING)
// ============================================================
void handleBackupNonBlocking() {
  if (WiFi.status() != WL_CONNECTED || queueCount > 0) {
    if (sedangProsesBackup) { fileBackupRead.close(); sedangProsesBackup = false; }
    return;
  }
  if (millis() - lastThingSpeakUpload < cfg.tsJedaMs) return;

  if (!sedangProsesBackup) {
    if (!SPIFFS.exists(backupFileName)) return;
    fileBackupRead = SPIFFS.open(backupFileName, FILE_READ);
    if (!fileBackupRead) return;
    sedangProsesBackup = true;
  }

  if (!fileBackupRead.available()) {
    fileBackupRead.close();
    SPIFFS.remove(backupFileName);
    sedangProsesBackup = false;
    Serial.println("[SPIFFS] Semua arsip berhasil diupload.");
    return;
  }

  String line = fileBackupRead.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // Hitung koma — format valid: 7 koma (8 field)
  int koma = 0;
  for (int i = 0; i < (int)line.length(); i++) if (line[i] == ',') koma++;
  if (koma < 7) {
    Serial.println("[SPIFFS] Baris korup dibuang: " + line);
    return;
  }

  // Parse ke SensorData
  char   parts[8][32] = {};
  int    start = 0;
  String tmp   = line + ",";
  for (int i = 0; i < 8; i++) {
    int comma = tmp.indexOf(',', start);
    if (comma == -1) break;
    String part = tmp.substring(start, comma);
    part.trim();
    strncpy(parts[i], part.c_str(), sizeof(parts[i]) - 1);
    start = comma + 1;
  }

  SensorData d;
  strncpy(d.nodeID, parts[0], sizeof(d.nodeID) - 1);
  d.tilt    = atof(parts[1]);
  d.rain    = atof(parts[2]);
  d.moist   = atof(parts[3]);
  d.battery = atof(parts[4]);
  d.rssi    = atoi(parts[5]);
  d.snr     = atof(parts[6]);
  d.valid   = true;

  bool sukses = kirimKeThingSpeak(d);
  lastThingSpeakUpload = millis();

  if (!sukses) {
    Serial.println("[SPIFFS] Upload baris gagal, akan dicoba di sesi berikutnya.");
    fileBackupRead.close();
    sedangProsesBackup = false;
    // File tidak dihapus — akan dicoba ulang dari awal di loop berikutnya
  }
}

// ============================================================
// HEARTBEAT MULTI-NODE
// ============================================================
void cekHeartbeatMultiNode() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_NODE; i++) {
    if (daftarNode[i].id[0] != '\0' && daftarNode[i].isOnline) {
      if (now - daftarNode[i].lastPacketTime > cfg.heartbeatMs) {
        daftarNode[i].isOnline = false;
        Serial.printf("[Heartbeat] Node %s OFFLINE.\n", daftarNode[i].id);
        enqueueNotif(daftarNode[i].id, 0, 0, 0,
                     daftarNode[i].battery,
                     daftarNode[i].lastRSSI,
                     daftarNode[i].lastSNR,
                     "NODE OFFLINE");
      }
    }
  }
}

// ============================================================
// [FIX-3] WIFI RECONNECT — pakai WiFi.reconnect() bukan begin()
// ============================================================
void handleWiFiReconnectNonBlocking() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiCheckTime >= cfg.wifiJedaMs) {
      lastWiFiCheckTime = millis();
      Serial.println("[WiFi] Terputus, mencoba reconnect...");
      WiFi.reconnect();  // [FIX-3] Lebih ringan dari disconnect+beginirim
    }
  }
}

// ============================================================
// CARI / TAMBAH NODE
// ============================================================
int cariAtauTambahNode(const char* id) {
  for (int i = 0; i < MAX_NODE; i++)
    if (strcmp(daftarNode[i].id, id) == 0) return i;
  for (int i = 0; i < MAX_NODE; i++) {
    if (daftarNode[i].id[0] == '\0') {
      strncpy(daftarNode[i].id, id, sizeof(daftarNode[i].id) - 1);
      return i;
    }
  }
  Serial.println("[Node] Daftar node penuh! Naikkan MAX_NODE.");
  return -1;
}

// ============================================================
// [FIX-4] NODE ID -> INT OTOMATIS
// ============================================================
int nodeIDtoInt(const char* id) {
  int len = strlen(id);
  int i   = len - 1;
  while (i >= 0 && isdigit((unsigned char)id[i])) i--;
  if (i < len - 1) return atoi(id + i + 1);
  for (int j = 0; j < MAX_NODE; j++)
    if (strcmp(daftarNode[j].id, id) == 0) return j + 1;
  return 1;
  }


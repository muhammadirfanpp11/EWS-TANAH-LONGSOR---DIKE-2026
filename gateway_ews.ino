#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // [FIX-6] HTTPS Telegram butuh WiFiClientSecure
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"

// 1. Konfigurasi WiFi & API Cloud
const char* ssid     = "Mikon";
const char* password = "12345678";
String apiKey        = "KCBA1A7JJJOOCIRR";
const char* serverName = "http://api.thingspeak.com/update";

// 2. Konfigurasi Telegram Bot
String tokenTelegram = "8638939390:AAEcT7bGQzJ_B6gkCSwOEBuryHizk5CCM1g";
String chatIDTelegram = "6548221896";

// 3. Konfigurasi SPI Antena Radio TTGO LoRa32 v2.1
#define SCK   5
#define MISO  19
#define MOSI  27
#define SS    18
#define RST   23
#define DIO0  26

// =========================
// DATA NODE
// =========================
String nodeID   = "UNKNOWN";
float  battery  = 0;
int    lastRSSI = 0;
float  lastSNR  = 0;

// Heartbeat
unsigned long lastPacketTime = 0;
bool nodeOnline = false;

// Backup
const char* backupFileName = "/backup_ews.txt";

// =========================
// [FIX-8] PROTOTYPE FUNGSI
// =========================
void prosesDataGateway(String node, float t, float r, float m, float bat, int rssi, float snr);
void kirimNotifikasiTelegram(String node, float t, float r, float m, float bat, int rssi, float snr);
void kirimBackupSPIFFS();
void reconnectWiFi();   // [FIX-5] Prototype reconnect WiFi

// =========================
// SETUP
// =========================
void setup() {
  Serial.begin(115200);

  // [F4-15] INISIALISASI BACKUP LOKAL (SPIFFS)
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Gagal! Sistem Backup Internal Offline.");
  } else {
    Serial.println("SPIFFS Berhasil Dimuat. Sistem Backup Internal Siap.");
  }

  // INISIALISASI KONEKSI WIFI
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan ke Jaringan WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Terhubung Sukses!");

  // INISIALISASI PIN BUS SPI UNTUK LORA
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(922E6)) {
    Serial.println("Modul LoRa Tidak Terdeteksi!");
    while (1);
  }
  Serial.println("Gateway LoRa Siap Menerima Data dari Node...");
}

// =========================
// LOOP
// =========================
void loop() {

  // [FIX-5] Reconnect otomatis jika WiFi putus
  reconnectWiFi();

  // Kirim backup SPIFFS setiap 30 detik jika WiFi terhubung
  if (WiFi.status() == WL_CONNECTED) {
    static unsigned long lastBackup = 0;
    if (millis() - lastBackup > 30000) {
      lastBackup = millis();
      kirimBackupSPIFFS();
    }
  }

  // Cek paket LoRa masuk
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String payload = "";
    while (LoRa.available()) {
      payload += (char)LoRa.read();
    }

    // [F4-13] Ambil metadata sinyal
    int   rssi = LoRa.packetRssi();
    float snr  = LoRa.packetSnr();

    Serial.println("\n--- PAKET BARU DITERIMA ---");
    Serial.println("Isi Payload: " + payload);
    Serial.printf("Kualitas Sinyal -> RSSI: %d dBm | SNR: %.2f dB\n", rssi, snr);

    // [F4-13] Parsing JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      nodeID  = doc["id"]      | "NODE01";
      float tilt  = doc["tilt"];
      float rain  = doc["rain"];
      float moist = doc["moist"];
      battery     = doc["battery"] | 0.0f;

      lastRSSI = rssi;
      lastSNR  = snr;
      lastPacketTime = millis();
      nodeOnline     = true;

      Serial.printf("Hasil Ekstraksi -> Miring: %.2f°, Hujan: %.2f mm, Lembap: %.2f%%\n",
                    tilt, rain, moist);

      prosesDataGateway(nodeID, tilt, rain, moist, battery, lastRSSI, lastSNR);

    } else {
      Serial.print("Gagal Parsing JSON: ");
      Serial.println(error.c_str());
    }
  }

  // Heartbeat: tandai node offline jika tidak ada paket >60 detik
  if (nodeOnline && (millis() - lastPacketTime > 60000)) {
    nodeOnline = false;
    Serial.println("NODE OFFLINE: Tidak ada paket selama 60 detik!");
  }
}

// =========================
// PROSES DATA GATEWAY
// =========================
// [FIX-1] Tambah kurung kurawal buka '{'
void prosesDataGateway(String node, float t, float r, float m, float bat, int rssi, float snr) {

  if (WiFi.status() == WL_CONNECTED) {

    // [F4-14] Kirim ke ThingSpeak
    HTTPClient http;
    String urlUpdate = String(serverName) +
                       "?api_key=" + apiKey +
                       "&field1="  + String(t) +
                       "&field2="  + String(r) +
                       "&field3="  + String(m) +
                       "&field4="  + String(rssi) +
                       "&field5="  + String(snr) +
                       "&field6="  + String(bat);
    // [FIX-4] field7 dihapus dari URL karena ThingSpeak tidak menyimpan string;
    //         Node ID sebaiknya dikodekan sebagai angka jika diperlukan.

    http.begin(urlUpdate);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
      Serial.println("Cloud Terupdate! Respon: " + String(httpResponseCode));
    } else {
      Serial.println("Gagal Kirim ke Cloud. Error: " + String(httpResponseCode));
    }
    http.end();

    // Kirim alert Telegram jika melewati threshold
    if (t >= 15.0 || r >= 50.0) {
      // [FIX-4] Teruskan semua parameter yang dibutuhkan
      kirimNotifikasiTelegram(node, t, r, m, bat, rssi, snr);
    }

  } else {
    // [F4-15] Failsafe: simpan ke SPIFFS jika internet mati
    Serial.println("Koneksi Internet Putus! Mengaktifkan Failsafe SPIFFS...");

    File backupFile = SPIFFS.open(backupFileName, FILE_APPEND);
    if (backupFile) {
      backupFile.printf("%s,%.2f,%.2f,%.2f,%.2f,%d,%.2f,%lu\n",
                        node.c_str(), t, r, m, bat, rssi, snr, millis());
      // [FIX-1(logika)] Tutup file setelah menulis
      backupFile.close();
      Serial.println("Data Tersimpan di Flash Memory ESP32.");
    } else {
      Serial.println("Gagal Menulis ke SPIFFS!");
    }
  }
}

// =========================
// KIRIM NOTIFIKASI TELEGRAM
// =========================
// [FIX-4] Fungsi menerima semua parameter yang dibutuhkan (m, bat, rssi, snr)
// [FIX-6] Menggunakan WiFiClientSecure untuk HTTPS
void kirimNotifikasiTelegram(String node, float t, float r, float m, float bat, int rssi, float snr) {

  WiFiClientSecure client;
  client.setInsecure(); // Lewati verifikasi sertifikat (cukup untuk ESP32 sederhana)

  HTTPClient http;

  String pesan = "🚨 *PERINGATAN DINI KAWASAN EWS* 🚨%0A";
  pesan += "Node : " + node + "%0A";
  pesan += "Kemiringan : " + String(t) + " Derajat%0A"; // Mengganti simbol ° jadi teks agar aman
  pesan += "Curah Hujan : " + String(r) + " mm%0A";
  pesan += "Kelembapan : " + String(m) + " persen%0A"; // Mengganti % jadi kata 'persen' agar tidak memicu error 400
  pesan += "Battery : " + String(bat) + " V%0A";
  pesan += "RSSI : " + String(rssi) + " dBm%0A";
  pesan += "SNR : " + String(snr) + " dB%0A";

  pesan.replace(" ", "%20");

  String urlTele = "https://api.telegram.org/bot" + tokenTelegram +
                   "/sendMessage?chat_id=" + chatIDTelegram +
                   "&text=" + pesan + "&parse_mode=Markdown";

  http.begin(client, urlTele);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.println("Notifikasi Telegram Berhasil Dikirim! Kode: " + String(httpCode));
  } else {
    Serial.println("Gagal Kirim Telegram. Error: " + String(httpCode));
  }

  http.end(); // [FIX-6] http.end() ada di dalam fungsinya sendiri, posisi benar
}

// =========================
// KIRIM ULANG BACKUP SPIFFS
// =========================
// [FIX-2] Fungsi ini berdiri sendiri, TIDAK di dalam fungsi Telegram
// [FIX-2(logika)] Upload ulang setiap baris ke ThingSpeak sebelum hapus file
void kirimBackupSPIFFS() {

  if (WiFi.status() != WL_CONNECTED) return;
  if (!SPIFFS.exists(backupFileName))  return;

  File file = SPIFFS.open(backupFileName, FILE_READ);
  if (!file) {
    Serial.println("Gagal membuka file backup.");
    return;
  }

  bool semuaBerhasil = true;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Format baris: node,t,r,m,bat,rssi,snr,millis
    // Upload ulang field1-field6 ke ThingSpeak
    int    idx   = 0;
    String parts[8];
    for (int i = 0; i < 8; i++) {
      int comma = line.indexOf(',', idx);
      if (comma == -1) { parts[i] = line.substring(idx); break; }
      parts[i] = line.substring(idx, comma);
      idx = comma + 1;
    }

    String urlUpdate = String(serverName) +
                       "?api_key=" + apiKey +
                       "&field1="  + parts[1] +
                       "&field2="  + parts[2] +
                       "&field3="  + parts[3] +
                       "&field4="  + parts[5] +
                       "&field5="  + parts[6] +
                       "&field6="  + parts[4];

    HTTPClient http;
    http.begin(urlUpdate);
    int code = http.GET();
    http.end();

    if (code > 0) {
      Serial.println("Backup baris berhasil dikirim ulang: " + line);
    } else {
      Serial.println("Gagal upload baris backup: " + String(code));
      semuaBerhasil = false;
      break; // Hentikan jika gagal; coba lagi di iterasi berikutnya
    }

    delay(1500); // ThingSpeak butuh jeda minimal 15 detik antar update di versi gratis
                 // Ganti ke 15000 jika pakai akun gratis ThingSpeak
  }

  file.close();

  // Hapus file backup hanya jika semua baris berhasil dikirim
  if (semuaBerhasil) {
    SPIFFS.remove(backupFileName);
    Serial.println("Semua backup berhasil dikirim. File dihapus.");
  }
}

// =========================
// RECONNECT WIFI
// =========================
// [FIX-5] Fungsi reconnect otomatis jika WiFi terputus
void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Terputus. Mencoba Reconnect...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnect WiFi Berhasil!");
    } else {
      Serial.println("\nReconnect Gagal. Akan dicoba lagi di loop berikutnya.");
    }
  }
}

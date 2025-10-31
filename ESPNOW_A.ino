#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// --- CONFIG: ganti peer_addr sesuai MAC lawan ---
uint8_t peer_addr[] = {0x98, 0xA3, 0x16, 0xEB, 0x65, 0x90};
const char *myName = "ESP32-A"; // ubah di device lain jadi "ESP32-B"

// --- Limits ---
const size_t MAX_V2_PAYLOAD = 1472; // upper safe limit for ESP-NOW V2

// --- globals untuk latency / ukuran terakhir ---
unsigned long sendStartMicros = 0;
size_t lastSendLen = 0;

// Callback ketika data terkirim (IDF5.x signature)
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  unsigned long txLatency = micros() - sendStartMicros;
  Serial.printf("Send Status: %s | TX Latency: %lu us | Packet len: %u\n",
                (status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail"),
                txLatency, (unsigned)lastSendLen);
}

// Callback saat menerima data
void OnDataRecv(const esp_now_recv_info *recvInfo, const uint8_t *data, int len) {
  if (!data || len <= 0) return;

  // tampilkan informasi pengirim
  if (recvInfo && recvInfo->src_addr) {
    const uint8_t *mac = recvInfo->src_addr;
    Serial.printf("\nFrom %02X:%02X:%02X:%02X:%02X:%02X - Received FULL payload: %d bytes\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], len);
  } else {
    Serial.printf("\nReceived FULL payload: %d bytes\n", len);
  }

  // Jika paket kecil kemungkinan ping (timestamp 8 byte) -> interpretasi RTT
  if ((size_t)len == sizeof(unsigned long)) {
    unsigned long t;
    memcpy(&t, data, sizeof(t));
    if (strcmp(myName, "ESP32-A") == 0) {
      // Hanya A mengukur RTT
      unsigned long rtt = micros() - t;
      Serial.printf("Ping RTT: %lu us (%lu ms)\n", rtt, rtt/1000);
    } else {
      // B cuma meng-echo timestamp kembali
      esp_now_send(peer_addr, data, len);
    }
    return;
  }

  // Cetak payload penuh. Gunakan Serial.write agar cepat dan tidak menambahkan konversi.
  // Jika payload berisi teks, ini akan tampil sebagai teks; jika binary, akan tampil raw bytes.
  Serial.println("=== PAYLOAD START ===");
  Serial.write(data, len);
  Serial.println(); // newline after payload
  Serial.println("=== PAYLOAD END ===\n");
}

void printMyMac() {
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    Serial.printf("My MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    Serial.println("Failed to read MAC");
  }
}

bool addPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peer_addr, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;
  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res == ESP_OK || res == ESP_ERR_ESPNOW_EXIST) return true;
  Serial.printf("esp_now_add_peer failed: %d\n", res);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // WiFi start
  WiFi.mode(WIFI_STA);
  esp_err_t r = esp_wifi_start();
  if (r != ESP_OK) Serial.printf("esp_wifi_start() returned %d\n", r);
  delay(100);

  printMyMac();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (1) delay(100);
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  if (!addPeer()) {
    Serial.println("Warning: failed to add peer (still starting).");
  }

  Serial.println("\nReady. Commands:");
  Serial.println("  sendword        -> send default 1024-byte word (A..Z)");
  Serial.println("  send <N>        -> send N bytes (N <= 1472)");
  Serial.println("  ping            -> RTT test (small timestamp packet)");
  Serial.println("  (Make sure peer_addr & myName configured)\n");
}

void loop() {
  if (!Serial.available()) {
    delay(10);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // parse commands
  if (line.equalsIgnoreCase("sendword")) {
    size_t N = 1024;
    if (N > MAX_V2_PAYLOAD) N = MAX_V2_PAYLOAD;
    char *buf = (char *)malloc(N);
    if (!buf) {
      Serial.println("Allocation failed");
      return;
    }
    // fill with A..Z repeated
    for (size_t i = 0; i < N; ++i) buf[i] = 'A' + (i % 26);
    sendStartMicros = micros();
    lastSendLen = N;
    esp_err_t res = esp_now_send(peer_addr, (uint8_t *)buf, N);
    Serial.printf("Sent word payload (%u bytes) -> result: %d\n", (unsigned)N, res);
    free(buf);
  }
  else if (line.startsWith("send ")) {
    // format: send <N>
    String part = line.substring(5);
    part.trim();
    long val = part.toInt();
    if (val <= 0) {
      Serial.println("Invalid size");
      return;
    }
    size_t N = (size_t)val;
    if (N > MAX_V2_PAYLOAD) {
      Serial.printf("Requested %u > MAX (%u). Limiting to MAX.\n", (unsigned)N, (unsigned)MAX_V2_PAYLOAD);
      N = MAX_V2_PAYLOAD;
    }
    char *buf = (char *)malloc(N);
    if (!buf) {
      Serial.println("Allocation failed");
      return;
    }
    // fill with readable pattern: line breaks every 64 chars to make it easy to inspect
    for (size_t i = 0; i < N; ++i) {
      buf[i] = 'A' + (i % 26);
      // optional: create visible segmentation by inserting newline occasionally
      // but be careful not to break binary tests; keeping plain A..Z is fine
    }
    sendStartMicros = micros();
    lastSendLen = N;
    esp_err_t res = esp_now_send(peer_addr, (uint8_t *)buf, N);
    Serial.printf("Sent payload (%u bytes) -> result: %d\n", (unsigned)N, res);
    free(buf);
  }
  else if (line.equalsIgnoreCase("ping")) {
    unsigned long t = micros();
    sendStartMicros = micros();
    lastSendLen = sizeof(t);
    esp_err_t res = esp_now_send(peer_addr, (uint8_t *)&t, sizeof(t));
    Serial.printf("Sent ping (%u bytes) -> result: %d\n", (unsigned)sizeof(t), res);
  }
  else {
    // send arbitrary small text
    String payload = String("[") + myName + "]: " + line;
    sendStartMicros = micros();
    lastSendLen = payload.length();
    esp_err_t res = esp_now_send(peer_addr, (uint8_t *)payload.c_str(), payload.length());
    Serial.printf("Sent text (%u bytes) -> result: %d\n", (unsigned)lastSendLen, res);
  }
}

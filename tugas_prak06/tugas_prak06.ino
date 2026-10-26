#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial Jaringan Wi-Fi
const char* ssid = "kar";
const char* password = "12345678";

// Konfigurasi Broker MQTT Publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782112_Full";

// Pembagian Topik Mandiri
const char* topicPublish   = "kampus/labiot/25782112/telemetri";
const char* topicSubscribe = "kampus/labiot/25782112/kendali";
const char* topicAlert = "kampus/labiot/25782112/alert";
const char* topicStatus = "kampus/labiot/25782112/status";

// LWT
const char* lwtMessage = "{\"status\":\"OFFLINE\",\"keterangan\":\"Perangkat Terputus Tak Terduga\"}";
const char* onlineMessage = "{\"status\":\"ONLINE\",\"keterangan\":\"Perangkat Beroperasi Normal\"}";

// Alokasi Pin Perangkat Keras
const byte dhtPin = 2; // Pin D4 (GPIO 2)
const byte ledPin = 5; // Pin D1 (GPIO 5)
#define DHTTYPE DHT22

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000; // Siklus kirim 4 detik

unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 4000;

// STATUS SISTEM
bool overheatAlertSent = false;
bool overheatCondition = false;

void callback(char* topic, byte* payload, unsigned int length) {
  char incomingBuffer[length + 1];
  memcpy(incomingBuffer, payload, length);
  incomingBuffer[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, incomingBuffer);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incomingBuffer);

  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  if (doc.containsKey("led")) {
    if (!overheatCondition) {
      int val = doc["led"];
      digitalWrite(ledPin, val == 1 ? HIGH : LOW);
      Serial.printf("--> [INDIKATOR] Status LED diubah: %s\n", val == 1 ? "MENYALA" : "PADAM");
    } else {
      Serial.println("--> [PERINGATAN] Kendali manual ditolak karena kondisi OVERHEAT!");
    }
  }

  Serial.println("----------------------------------------");
}

void reconnect() {
  if (client.connected()) {
    return;
  }

  unsigned long now = millis();

  if (lastReconnectAttempt != 0 && now - lastReconnectAttempt < reconnectInterval) {
    return;
  }

  lastReconnectAttempt = now;

  Serial.print("Menghubungkan kembali ke Broker...");

  if (client.connect(clientId, topicStatus, 1, true, lwtMessage)) {
    Serial.println(" Sukses Terhubung!");

    lastReconnectAttempt = 0;

    client.subscribe(topicSubscribe);
    Serial.printf("Subskripsi Topik: %s\n", topicSubscribe);

    client.publish(topicStatus, onlineMessage, true);
    Serial.println("[STATUS] Perangkat ONLINE");
  } else {
    Serial.printf("Gagal (rc=%d). Akan mencoba kembali...\n", client.state());
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan Wi-Fi");

  unsigned long lastDotTime = 0;

  while (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();

    if (now - lastDotTime >= 500) {
      lastDotTime = now;
      Serial.print(".");
    }

    yield();
  }

  Serial.println("\nWi-Fi Terkoneksi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }

  client.loop(); // Memelihara sesi TCP dan memproses callback masuk

  unsigned long now = millis();

  if (now - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {

      // Pemeriksaan kondisi suhu
      if (t > 31.0) {
        overheatCondition = true;

        // LED otomatis menyala ketika suhu melebihi 31°C
        digitalWrite(ledPin, HIGH);

        // Alert hanya dikirim satu kali saat kondisi bahaya pertama terdeteksi
        if (!overheatAlertSent) {
          StaticJsonDocument<256> alertDoc;
          alertDoc["status"] = "BAHAYA";
          alertDoc["parameter"] = "OVERHEAT";
          alertDoc["nilai_suhu"] = serialized(String(t, 2));

          char alertBuffer[256];
          serializeJson(alertDoc, alertBuffer);

          bool alertSuccess = client.publish(topicAlert, alertBuffer);

          if (alertSuccess) {
            Serial.printf("[ALERT TERKIRIM] %s\n", alertBuffer);
            overheatAlertSent = true;
          } else {
            Serial.println("[ERROR] Gagal memublikasikan alert!");
          }
        }
      } else {
        overheatCondition = false;
        overheatAlertSent = false;
      }

      StaticJsonDocument<256> docOut;
      docOut["node_id"] = clientId;
      docOut["suhu"] = serialized(String(t, 2));
      docOut["kelembapan"] = serialized(String(h, 2));
      docOut["status_led"] = digitalRead(ledPin);

      // Klasifikasi kondisi suhu
      if (t <= 31.0) {
        docOut["kondisi_suhu"] = "NORMAL";
      } else {
        docOut["kondisi_suhu"] = "PANAS";
      }

      char buffer[256];
      serializeJson(docOut, buffer);

      bool success = client.publish(topicPublish, buffer);

      if (success) {
        Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
      } else {
        Serial.println("[ERROR] Gagal memublikasikan telemetri!");
      }
    }
  }
}
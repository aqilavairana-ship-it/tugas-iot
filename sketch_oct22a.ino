
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <BH1750.h>

//========== PIN ESP32 ==========
#define DHTPIN 4
#define DHTTYPE DHT22
#define SOIL_PIN 34
#define RELAY_PIN 27

// Sesuaikan dengan jenis relay yang digunakan
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ========== KALIBRASI SOIL MOISTURE ==========
#define SOIL_DRY_RAW 3000
#define SOIL_WET_RAW 1500

// ========== WIFI ==========
const char* WIFI_SSID = "kila";
const char* WIFI_PASS = "14444444";

// ========== MQTT ==========
const char* MQTT_HOST = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Gunakan ID unik yang sama dengan dashboard web
const char* ID_PERANGKAT = "cabai-rawit-1234";

char topicStatus[80];
char topicCmd[80];

WiFiClient espClient;
PubSubClient mqtt(espClient);

DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;

// ========== STATUS SISTEM ==========
bool pompaMenyala = false;
bool modeManual = false;
bool kirimSegera = false;
bool bh1750Siap = false;

int gTanah = 0;
int kondisiAktif = 0;

float gSuhu = 0;
float gLembap = 0;
float gLux = 0;

unsigned long tSensor = 0;
unsigned long tReconnect = 0;

// ========== BACA KELEMBAPAN TANAH ==========
int bacaTanahPersen() {
  int raw = analogRead(SOIL_PIN);

  int persen = map(
    raw,
    SOIL_DRY_RAW,
    SOIL_WET_RAW,
    0,
    100);

  return constrain(persen, 0, 100);
}

// ========== KONTROL POMPA ==========
void setPompa(bool nyala) {
  pompaMenyala = nyala;

  digitalWrite(
    RELAY_PIN,
    nyala ? RELAY_ON : RELAY_OFF);
}

// ========== LOGIKA 3 KONDISI ==========
void evaluasiOtomatis() {
  if (modeManual) {
    kondisiAktif = 0;
    return;
  }

  // Kondisi 1:
  // Tanah < 50%, suhu < 25 C, cahaya < 5000 lux
  if (gTanah < 50 && gSuhu < 25 && gLux < 5000) {

    setPompa(true);
    kondisiAktif = 1;
  }

  // Kondisi 2:
  // Tanah 50-60%, suhu 25-35 C,
  // cahaya 1000-5000 lux
  else if (gTanah >= 50 && gTanah <= 60 && gSuhu >= 25 && gSuhu <= 35 && gLux >= 1000 && gLux <= 5000) {

    setPompa(false);
    kondisiAktif = 2;
  }

  // Kondisi 3:
  // Tanah > 60%, suhu < 25 C, cahaya > 5000 lux
  else if (gTanah > 60 && gSuhu < 25 && gLux > 5000) {

    setPompa(false);
    kondisiAktif = 3;
  }

  // Tidak memenuhi kondisi 1, 2, atau 3
  else {
    setPompa(false);
    kondisiAktif = 0;
  }
}

// ========== TERIMA PERINTAH DARI WEB ==========
void onPesan(
  char* topic,
  byte* payload,
  unsigned int len) {
  String s;

  for (unsigned int i = 0; i < len; i++) {
    s += (char)payload[i];
  }

  Serial.println("Perintah web: " + s);

  if (s.indexOf("\"mode\":\"manual\"") >= 0) {
    modeManual = true;
    kondisiAktif = 0;
  }

  if (s.indexOf("\"mode\":\"auto\"") >= 0) {
    modeManual = false;
    evaluasiOtomatis();
  }

  if (modeManual) {
    if (s.indexOf("\"pompa\":1") >= 0) {
      setPompa(true);
    }

    if (s.indexOf("\"pompa\":0") >= 0) {
      setPompa(false);
    }
  }

  kirimSegera = true;
}

// ========== KONEKSI MQTT ==========
void sambungMqtt() {
  if (mqtt.connected()) return;

  if (millis() - tReconnect < 5000) return;

  tReconnect = millis();

  String cid = "esp32-cabai-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqtt.connect(cid.c_str())) {
    mqtt.subscribe(topicCmd);
    Serial.println("MQTT tersambung");
  } else {
    Serial.printf(
      "MQTT gagal, kode: %d\n",
      mqtt.state());
  }
}

// ========== KIRIM DATA DASHBOARD ==========
void kirimStatus() {
  if (!mqtt.connected()) return;

  char buf[220];

  snprintf(
    buf,
    sizeof(buf),
    "{\"tanah\":%d,\"suhu\":%.1f,\"lembap\":%.1f,"
    "\"lux\":%.0f,\"pompa\":%d,\"mode\":\"%s\","
    "\"kondisi\":%d}",
    gTanah,
    gSuhu,
    gLembap,
    gLux,
    pompaMenyala ? 1 : 0,
    modeManual ? "manual" : "auto",
    kondisiAktif);

  bool sukses = mqtt.publish(topicStatus, buf);

  Serial.println(
    sukses
      ? String("Status: ") + buf
      : "Gagal mengirim status MQTT");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);

  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(RELAY_PIN, OUTPUT);
  setPompa(false);

  analogReadResolution(12);
  dht.begin();

  // BH1750: SDA = GPIO 15, SCL = GPIO 16
  Wire.begin(15, 16);

  bh1750Siap = lightMeter.begin(
    BH1750::CONTINUOUS_HIGH_RES_MODE);

  if (bh1750Siap) {
    Serial.println("BH1750 siap");
  } else {
    Serial.println(
      "BH1750 gagal terdeteksi; periksa kabel");
  }

  snprintf(
    topicStatus,
    sizeof(topicStatus),
    "%s/status",
    ID_PERANGKAT);

  snprintf(
    topicCmd,
    sizeof(topicCmd),
    "%s/cmd",
    ID_PERANGKAT);

  Serial.println("Menghubungkan WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi tersambung");
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onPesan);
}

// ========== LOOP ==========
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(10);
    return;
  }

  sambungMqtt();
  mqtt.loop();

  if (kirimSegera) {
    kirimSegera = false;
    kirimStatus();
  }

  if (millis() - tSensor < 2000) return;

  tSensor = millis();

  // Baca sensor tanah
  gTanah = bacaTanahPersen();

  // Baca DHT22
  float suhu = dht.readTemperature();
  float lembap = dht.readHumidity();

  if (!isnan(suhu) && !isnan(lembap)) {
    gSuhu = suhu;
    gLembap = lembap;
  } else {
    Serial.println("Gagal membaca DHT22");
  }

  // Baca BH1750
  if (bh1750Siap) {
    float lux = lightMeter.readLightLevel();

    if (lux >= 0) {
      gLux = lux;
    } else {
      Serial.println("Gagal membaca BH1750");
    }
  }

  // Evaluasi tiga kondisi otomatis
  evaluasiOtomatis();

  // Kirim data ke dashboard
  kirimStatus();
}
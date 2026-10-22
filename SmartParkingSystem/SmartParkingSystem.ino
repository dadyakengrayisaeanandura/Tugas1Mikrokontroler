#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>

const char* WIFI_SSID = "NAMA_WIFI";
const char* WIFI_PASS = "PASSWORD_WIFI";

#define PIN_IR_IN   4
#define PIN_IR_OUT  5
#define PIN_SERVO   18
#define PIN_LDR     1
#define PIN_LED     6
#define PIN_BUZZER  7

const bool IR_AKTIF_LOW = true;

const unsigned long BLOCK_STABLE_MS = 10;
const unsigned long CLEAR_STABLE_MS = 150;
const unsigned long JENDELA_URUTAN = 3000;

const int LDR_GELAP = 1200;
const int LDR_TERANG = 1800;
const unsigned long LDR_INTERVAL = 200;

const int SERVO_CLOSED = 0;
const int SERVO_OPEN = 90;
const unsigned long OPEN_TIME = 3000;

const int MAX_MOBIL = 3;
const unsigned long BUZZER_TIME = 1500;

const unsigned long WIFI_TIMEOUT = 10000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

struct IRSensor {
  uint8_t pin;
  volatile unsigned long tChange;
  bool blocked;
};

IRSensor irIn = {PIN_IR_IN, 0, false};
IRSensor irOut = {PIN_IR_OUT, 0, false};

Servo servo;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WebServer server(80);

bool gateOpen = false;
int jumlahMobil = 0;
bool malam = false;
int nilaiLdrTerakhir = 0;
unsigned long tLdr = 0;
unsigned long tTerakhirAda = 0;
bool buzzerOn = false;
unsigned long tBuzzer = 0;

bool prevIn = false, prevOut = false;
unsigned long tInBlock = 0, tOutBlock = 0;
bool menungguOut = false, menungguIn = false;

bool serverStarted = false;
unsigned long tWifiRetry = 0;

const char HALAMAN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Monitoring Parkir</title>
<style>
  body { font-family: Arial, sans-serif; background:#f2f4f7; margin:0; padding:20px; color:#222; }
  .kartu { max-width:420px; margin:0 auto; background:#fff; border-radius:14px; padding:24px;
           box-shadow:0 2px 8px rgba(0,0,0,.12); text-align:center; }
  h1 { font-size:20px; margin:0 0 16px; }
  .angka { font-size:72px; font-weight:bold; margin:8px 0; }
  .status { display:inline-block; padding:8px 18px; border-radius:20px; color:#fff; font-weight:bold; }
  .ok { background:#2e9e5b; } .penuh { background:#d64545; }
  table { width:100%; margin-top:20px; border-collapse:collapse; }
  td { padding:8px 4px; border-top:1px solid #e5e5e5; text-align:left; }
  td:last-child { text-align:right; font-weight:bold; }
  .kecil { font-size:12px; color:#888; margin-top:14px; }
</style>
</head>
<body>
<div class="kartu">
  <h1>Monitoring Parkir</h1>
  <div>Mobil di dalam</div>
  <div class="angka"><span id="jumlah">-</span>/<span id="maks">-</span></div>
  <div id="status" class="status ok">-</div>
  <table>
    <tr><td>Slot kosong</td><td id="slot">-</td></tr>
    <tr><td>Gerbang</td><td id="gerbang">-</td></tr>
    <tr><td>Kondisi cahaya</td><td id="cahaya">-</td></tr>
    <tr><td>Nilai LDR (ADC)</td><td id="ldr">-</td></tr>
  </table>
  <div class="kecil" id="info">Menghubungkan...</div>
</div>
<script>
async function ambil() {
  try {
    const r = await fetch('/data');
    const d = await r.json();
    document.getElementById('jumlah').textContent = d.jumlah;
    document.getElementById('maks').textContent = d.maks;
    document.getElementById('slot').textContent = d.slot;
    document.getElementById('gerbang').textContent = d.gerbang;
    document.getElementById('cahaya').textContent = d.malam ? 'Malam (LED nyala)' : 'Siang (LED mati)';
    document.getElementById('ldr').textContent = d.ldr;
    const s = document.getElementById('status');
    s.textContent = d.penuh ? 'PARKIR PENUH' : 'TERSEDIA';
    s.className = 'status ' + (d.penuh ? 'penuh' : 'ok');
    document.getElementById('info').textContent = 'Diperbarui: ' + new Date().toLocaleTimeString();
  } catch (e) {
    document.getElementById('info').textContent = 'Koneksi terputus...';
  }
}
ambil();
setInterval(ambil, 1000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", HALAMAN);
}

void handleData() {
  String j = "{";
  j += "\"jumlah\":" + String(jumlahMobil);
  j += ",\"maks\":" + String(MAX_MOBIL);
  j += ",\"slot\":" + String(MAX_MOBIL - jumlahMobil);
  j += ",\"penuh\":" + String(jumlahMobil >= MAX_MOBIL ? "true" : "false");
  j += ",\"gerbang\":\"" + String(gateOpen ? "BUKA" : "TUTUP") + "\"";
  j += ",\"malam\":" + String(malam ? "true" : "false");
  j += ",\"ldr\":" + String(nilaiLdrTerakhir);
  j += "}";
  server.send(200, "application/json", j);
}

void IRAM_ATTR isrIn() { irIn.tChange = millis(); }
void IRAM_ATTR isrOut() { irOut.tChange = millis(); }

bool bacaRaw(const IRSensor &s) {
  return (digitalRead(s.pin) == LOW) == IR_AKTIF_LOW;
}

void updateSensor(IRSensor &s) {
  bool raw = bacaRaw(s);
  if (raw == s.blocked) return;
  unsigned long diam = millis() - s.tChange;
  if (diam >= (raw ? BLOCK_STABLE_MS : CLEAR_STABLE_MS)) s.blocked = raw;
}

void showCount() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("MOBIL DI DALAM");

  oled.setTextSize(3);
  oled.setCursor(0, 16);
  oled.print(jumlahMobil);
  oled.print("/");
  oled.print(MAX_MOBIL);

  if (jumlahMobil >= MAX_MOBIL) {
    oled.setTextSize(2);
    oled.setCursor(56, 16);
    oled.print("PARKIR");
    oled.setCursor(56, 34);
    oled.print("PENUH");
  } else {
    oled.setTextSize(1);
    oled.setCursor(0, 44);
    oled.print("Slot kosong: ");
    oled.print(MAX_MOBIL - jumlahMobil);
  }

  oled.setTextSize(1);
  oled.setCursor(0, 56);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP());
  } else {
    oled.print("WiFi: tidak terhubung");
  }

  oled.display();
}

void bukaGerbang() {
  servo.write(SERVO_OPEN);
  gateOpen = true;
  tTerakhirAda = millis();
}

void bunyikanBuzzer() {
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerOn = true;
  tBuzzer = millis();
}

void mulaiServerJikaPerlu() {
  if (!serverStarted && WiFi.status() == WL_CONNECTED) {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();
    serverStarted = true;
    Serial.print("Web siap: http://");
    Serial.println(WiFi.localIP());
    showCount();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  Wire.begin(8, 9);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED tidak ditemukan");
  } else {
    showCount();
  }

  pinMode(PIN_IR_IN, INPUT_PULLUP);
  pinMode(PIN_IR_OUT, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  servo.setPeriodHertz(50);
  servo.attach(PIN_SERVO, 500, 2400);
  servo.write(SERVO_CLOSED);

  attachInterrupt(digitalPinToInterrupt(PIN_IR_IN), isrIn, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_IR_OUT), isrOut, CHANGE);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Menghubungkan WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    mulaiServerJikaPerlu();
  } else {
    Serial.println("WiFi belum terhubung, sistem tetap berjalan");
  }

  Serial.println("Siap");
}

void loop() {
  if (serverStarted) {
    server.handleClient();
  } else if (millis() - tWifiRetry > WIFI_RETRY_INTERVAL) {
    tWifiRetry = millis();
    mulaiServerJikaPerlu();
  }

  updateSensor(irIn);
  updateSensor(irOut);

  unsigned long tNow = millis();
  bool adaMobil = irIn.blocked || irOut.blocked;
  if (adaMobil) tTerakhirAda = tNow;

  bool inBaru = irIn.blocked && !prevIn;
  bool outBaru = irOut.blocked && !prevOut;
  prevIn = irIn.blocked;
  prevOut = irOut.blocked;

  if (inBaru) {
    if (menungguIn && tNow - tOutBlock <= JENDELA_URUTAN) {
      menungguIn = false;
      if (jumlahMobil > 0) jumlahMobil--;
      Serial.printf("Mobil keluar, jumlah=%d\n", jumlahMobil);
      showCount();
    } else if (jumlahMobil >= MAX_MOBIL) {
      Serial.println("Parkir penuh, mobil ditolak");
      bunyikanBuzzer();
    } else {
      Serial.println("Mobil datang dari luar, gerbang buka");
      bukaGerbang();
      menungguOut = true;
      tInBlock = tNow;
    }
  }

  if (outBaru) {
    if (menungguOut && tNow - tInBlock <= JENDELA_URUTAN) {
      menungguOut = false;
      jumlahMobil++;
      Serial.printf("Mobil masuk, jumlah=%d\n", jumlahMobil);
      showCount();
    } else {
      Serial.println("Mobil datang dari dalam, gerbang buka");
      bukaGerbang();
      menungguIn = true;
      tOutBlock = tNow;
    }
  }

  if (gateOpen && !adaMobil && tNow - tTerakhirAda > OPEN_TIME) {
    servo.write(SERVO_CLOSED);
    gateOpen = false;
    Serial.println("Gerbang tutup");
  }

  if (buzzerOn && tNow - tBuzzer >= BUZZER_TIME) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOn = false;
  }

  if (tNow - tLdr >= LDR_INTERVAL) {
    tLdr = tNow;
    int nilaiLdr = analogRead(PIN_LDR);
    nilaiLdrTerakhir = nilaiLdr;

    if (!malam && nilaiLdr < LDR_GELAP) {
      malam = true;
      digitalWrite(PIN_LED, HIGH);
      Serial.printf("Malam, LED nyala (LDR=%d)\n", nilaiLdr);
    } else if (malam && nilaiLdr > LDR_TERANG) {
      malam = false;
      digitalWrite(PIN_LED, LOW);
      Serial.printf("Siang, LED mati (LDR=%d)\n", nilaiLdr);
    }
  }
}

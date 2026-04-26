// ============================================================
//  TERRARIO MONITOR
// ============================================================

#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <InfluxDbClient.h>
#include "secrets.h"

// ============================================================
// WIFI + SERVER
// ============================================================
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
ESP8266WebServer server(80);

// ============================================================
// INFLUXDB
// ============================================================
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
Point point("terrario");

// ============================================================
// SOGLIE DI DEFAULT
// ============================================================
float TEMP_MIN      = 18.0;
float TEMP_MAX      = 30.0;
float HUM_MIN       = 40.0;
float HUM_MAX       = 80.0;
int   LIGHT_MIN     = 600;
float WIFI_MIN_RSSI = -50.0;

// ============================================================
// PIN
// ============================================================
#define DHTPIN    D4
#define BUZZER    D3
#define LED_R     D6
#define LED_G     D0
#define LED_B     D8
#define PHOTO     A0
#define LED_EXTERNAL   D5

#define DHTTYPE       DHT11
#define READ_INTERVAL 5000UL

// ============================================================
// STATO GLOBALE
// ============================================================
bool systemActive = false;
bool buzzerSilenced = false;
bool alarmActive = false;
float lastT = 0;
float lastH = 0;
int lastL = 0;
int lastRssi = 0;

// ============================================================
// DHT e DISPLAY
// ============================================================
DHT dht = DHT(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============================================================
// LED 
// ============================================================
enum LedColor { OFF, GREEN, YELLOW, BLUE, PURPLE, RED, ORANGE };

const char* ledColorName(LedColor c) {
  switch (c) {
    case GREEN:  return "GREEN";
    case YELLOW: return "YELLOW";
    case BLUE:   return "BLUE";
    case PURPLE: return "PURPLE";
    case RED:    return "RED";
    case ORANGE: return "ORANGE";
    default:     return "OFF";
  }
}

void setLed(LedColor c) {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);

  switch (c) {
    case GREEN:  digitalWrite(LED_G, HIGH); break;
    case YELLOW: digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); break;
    case BLUE:   digitalWrite(LED_B, HIGH); break;
    case PURPLE: digitalWrite(LED_R, HIGH); digitalWrite(LED_B, HIGH); break;
    case RED:    digitalWrite(LED_R, HIGH); break;
    case ORANGE: digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); break;
    default: break;
  }
}

// ============================================================
// BUZZER
// ============================================================
void buzzerOn() {
  if (!buzzerSilenced) {
    digitalWrite(BUZZER, LOW); 
  } else {
    digitalWrite(BUZZER, HIGH);
  }
}

void buzzerOff() {
  digitalWrite(BUZZER, HIGH);
}

void buzzerForceOn() {
  digitalWrite(BUZZER, LOW);
}

// ============================================================
// EXTERNAL LED
// ============================================================
void externalLedOn() {
  digitalWrite(LED_EXTERNAL, LOW);
}

void externalLedOff() {
  digitalWrite(LED_EXTERNAL, HIGH);
}

// ============================================================
// WIFI
// ============================================================
void connectWiFi() {
  Serial.print("Connessione WiFi...  ");
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  WiFi.begin(ssid, pass);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempts > 40) {
      Serial.println(" ERRORE (timeout 20s)");

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("ERRORE WIFI");
      lcd.setCursor(0, 1);
      lcd.print("Riavviare");

      setLed(RED);
      buzzerForceOn();

      while (true) { delay(1000); }
    }
  }

  Serial.println(" OK");
  Serial.print("IP: ");   Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
}

// ============================================================
// LCD
// ============================================================
void lcdUpdate(float t, float h, int l) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print((int)t);
  lcd.print((char)223);
  lcd.print("C H:");
  lcd.print((int)h);
  lcd.print("%");

  lcd.setCursor(0, 1);
  lcd.print("L:");
  lcd.print(l);
}

void lcdOff() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SYSTEM OFF");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP());
}

// ============================================================
// DHT
// ============================================================
bool readDHT(float &t, float &h) {
  h = dht.readHumidity();
  t = dht.readTemperature();

  if (isnan(t) || isnan(h)) {
    Serial.println(F("Failed to read from DHT sensor!"));
    setLed(RED);
    buzzerForceOn();
    return false;
  }

  return true;
}

// ============================================================
// INFLUXDB
// ============================================================
void writeDB(float t, float h, int l, int rssi, int c) {
  //point.clearFields();
  point.addField("t",    t);
  point.addField("h",    h);
  point.addField("l",    l);
  point.addField("rssi", rssi);
  point.addField("led",  c);

  if (client.writePoint(point)) {
    Serial.println("DB: OK");
  } else {
    Serial.print("DB: ERRORE - ");
    Serial.println(client.getLastErrorMessage());
  }
}

// ============================================================
// PAGINE WEB – CSS
// ============================================================
const String CSS = R"(
<style>

  body {
    font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: #f3f4f6;
    color: #111827;
    margin: 0;
    padding: 20px;
  }

  .container {
    max-width: 900px;
    margin: auto;
  }

  .header {
    text-align: center;
    margin-bottom: 24px;
  }

  .header h1 {
    color: #0ea5e9;
    font-size: 2rem;
    margin: 0;
    font-weight: 700;
  }

  .header small {
    color: #6b7280;
    font-size: 0.9rem;
  }

  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
    gap: 18px;
    margin-bottom: 20px;
  }

  .card {
    background: #ffffff;
    padding: 20px;
    border-radius: 16px;
    border: 1px solid #d1d5db;
    box-shadow: 0 4px 12px rgba(0,0,0,0.06);
  }

  .label {
    color: #6b7280;
    font-size: 0.8rem;
    text-transform: uppercase;
    letter-spacing: 1px;
    display: flex;
    justify-content: space-between;
    align-items: center;
  }

  .val {
    font-size: 2rem;
    font-weight: 700;
    margin: 12px 0 6px;
  }

  .sub {
    font-size: 0.85rem;
    color: #6b7280;
    margin-top: 4px;
  }

  .badge {
    padding: 5px 10px;
    border-radius: 999px;
    font-size: 0.75rem;
    font-weight: 700;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    margin-top: 6px;
  }

  .ok {
    background: #dcfce7;
    color: #166534;
  }

  .warn {
    background: #fef9c3;
    color: #854d0e;
  }

  .alert {
    background: #fee2e2;
    color: #991b1b;
  }

  label.sub {
    font-size: 0.85rem;
    font-weight: 600;
    color: #374151;
    margin-bottom: 4px;
    display: block;
  }

  input {
    width: 100%;
    padding: 12px 14px;
    margin: 8px 0 14px;
    border-radius: 12px;
    border: 1px solid #d1d5db;
    background: #ffffff;
    color: #111827;
    font-size: 1rem;
    transition: all 0.25s ease;
    box-shadow: 0 1px 2px rgba(0,0,0,0.05);
  }

  input:hover {
    border-color: #0ea5e9;
  }

  input:focus {
    outline: none;
    border-color: #0ea5e9;
    box-shadow: 0 0 0 3px rgba(14,165,233,0.25);
  }

  .btn {
    display: block;
    width: 100%;
    padding: 14px;
    border-radius: 12px;
    border: none;
    font-weight: 600;
    cursor: pointer;
    text-decoration: none;
    margin-top: 10px;
    font-size: 1rem;
    text-align: center;
    transition: all 0.2s ease;
  }

  .btn:hover {
    transform: translateY(-2px);
    box-shadow: 0 6px 14px rgba(0,0,0,0.12);
  }

  .btn-blue   { background: #0284c7; color: white; }
  .btn-green  { background: #16a34a; color: white; }
  .btn-red    { background: #dc2626; color: white; }
  .btn-orange { background: #d97706; color: white; }

  .btn-row {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
  }

</style>
)";


// ============================================================
// PAGINA CONFIGURAZIONE
// ============================================================
String configPage() {
  return String("<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>") + CSS +
    "<body><div class='container'>"
      "<div class='header'>"
        "<h1>&#x1F40D; Terrario Setup</h1>"
        "<small>Imposta le soglie prima di avviare il monitor</small>"
      "</div>"

      "<form method='POST' action='/start' class='card'>"
        "<div class='label'><span>Temperatura (&#x2103;)</span><span class='icon'>&#x1F321;</span></div>"
        "<label for='tmin' class='sub'>Temperatura minima</label>"
        "<input name='tmin' type='number' step='0.1' value='18'>"
        "<label for='tmax' class='sub'>Temperatura massima</label>"
        "<input name='tmax' type='number' step='0.1' value='30'>"

        "<div class='label' style='margin-top:8px;'><span>Umidita' (%)</span><span class='icon'>&#x1F4A7;</span></div>"
        "<label for='hmin' class='sub'>Umidita' minima</label>"
        "<input name='hmin' type='number' step='0.1' value='40'>"
        "<label for='hmax' class='sub'>Umidita' massima</label>"
        "<input name='hmax' type='number' step='0.1' value='80'>"

        "<div class='label' style='margin-top:8px;'><span>Luce</span><span class='icon'>&#x1F4A1;</span></div>"
        "<label class='sub'>Luce minima</label>"
        "<input name='lmin' type='number' value='600'>"

        "<button class='btn btn-green' type='submit'>&#x25B6;&#xFE0F; AVVIA SISTEMA</button>"
      "</form>"

    "</div></body></html>";
}

// ============================================================
// DASHBOARD
// ============================================================
String dashboard() {

  auto getBadge = [](float val, float min, float max) {
    if (val < min) return "<span class='badge warn'>&#x26A0;&#xFE0F; BASSO</span>";
    if (val > max) return "<span class='badge alert'>&#x1F525; ALTO</span>";
    return "<span class='badge ok'>&#x2705; OK</span>";
  };

  String buzzerState = buzzerSilenced ? "Silenziato" : "Attivo";
  String buzzerEmoji = buzzerSilenced ? "&#x1F507;" : "&#x1F50A;";
  String buzzerBadgeClass = buzzerSilenced ? "warn" : "ok";

  return String("<html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='3'>") + CSS +
    "<body><div class='container'>"

      "<div class='header'>"
        "<h1>&#x1F40D; Terrario Monitor</h1>"
        "<small>Dashboard in tempo reale</small>"
      "</div>"

      "<div class='grid'>"

        "<div class='card'>"
          "<div class='label'><span>Temperatura</span><span>&#x1F321;</span></div>"
          "<div class='val'>" + String(lastT, 1) + "&#x2103;</div>"
          + getBadge(lastT, TEMP_MIN, TEMP_MAX) +
          "<div class='sub'>Range: " + String(TEMP_MIN) + " - " + String(TEMP_MAX) + "</div>"
        "</div>"

        "<div class='card'>"
          "<div class='label'><span>Umidità</span><span>&#x1F4A7;</span></div>"
          "<div class='val'>" + String(lastH, 1) + "%</div>"
          + getBadge(lastH, HUM_MIN, HUM_MAX) +
          "<div class='sub'>Range: " + String(HUM_MIN) + " - " + String(HUM_MAX) + "</div>"
        "</div>"

        "<div class='card'>"
          "<div class='label'><span>Luce</span><span>&#x1F4A1;</span></div>"
          "<div class='val'>" + String(lastL) + "</div>"
          + (lastL < LIGHT_MIN
              ? "<span class='badge warn'>&#x1F319; BUIO</span>"
              : "<span class='badge ok'>&#x2600;&#xFE0F; OK</span>") +
          "<div class='sub'>Minimo: " + String(LIGHT_MIN) + "</div>"
        "</div>"

        "<div class='card'>"
          "<div class='label'><span>WiFi</span><span>&#x1F4F6;</span></div>"
          "<div class='val'>" + String(lastRssi) + " dBm</div>"
          + getBadge(lastRssi, WIFI_MIN_RSSI, 1000) +
          "<div class='sub'>Soglia: " + String(WIFI_MIN_RSSI) + " dBm</div>"
        "</div>"

        "<div class='card'>"
          "<div class='label'><span>Allarme</span><span>&#x1F50A;</span></div>"
          "<div class='val'>" + buzzerEmoji + " " + buzzerState + "</div>"
          "<span class='badge " + buzzerBadgeClass + "'>" +
            (alarmActive ? "Allarme attivo" : "Nessun allarme") +
          "</span>"
        "</div>"

      "</div>"

      "<div class='card'>"
        "<div class='label'><span>Controlli</span><span>&#x2699;&#xFE0F;</span></div>"
        "<div class='btn-row'>"
          "<a class='btn btn-orange' href='/silence'>&#x1F507; Silenzia</a>"
          "<a class='btn btn-blue' href='/unsilence'>&#x1F50A; Riattiva</a>"
        "</div>"
        "<a class='btn btn-red' href='/stop' style='margin-top:12px;'>&#x23F9;&#xFE0F; Stop Sistema</a>"
      "</div>"

    "</div></body></html>";
}


// ============================================================
// ROUTE
// ============================================================
void handleRoot() {
  if (systemActive) {
    server.sendHeader("Location", "/dashboard");
    server.send(302);
  } else {
    server.send(200, "text/html", configPage());
  }
}

void handleStart() {
  String tmin = server.arg("tmin");
  String tmax = server.arg("tmax");
  String hmin = server.arg("hmin");
  String hmax = server.arg("hmax");
  String lmin = server.arg("lmin");

  TEMP_MIN  = tmin.length() > 0 ? tmin.toFloat() : 18.0;
  TEMP_MAX  = tmax.length() > 0 ? tmax.toFloat() : 30.0;
  HUM_MIN   = hmin.length() > 0 ? hmin.toFloat() : 40.0;
  HUM_MAX   = hmax.length() > 0 ? hmax.toFloat() : 80.0;
  LIGHT_MIN = lmin.length() > 0 ? lmin.toInt()   : 300;

  Serial.println("--- SOGLIE IMPOSTATE ---");
  Serial.print("  Temp:  "); Serial.print(TEMP_MIN);  Serial.print(" - "); Serial.println(TEMP_MAX);
  Serial.print("  Hum:   "); Serial.print(HUM_MIN);   Serial.print(" - "); Serial.println(HUM_MAX);
  Serial.print("  Luce:  "); Serial.println(LIGHT_MIN);
  Serial.println("------------------------");

  systemActive   = true;
  buzzerSilenced = false;
  buzzerOff();

  Serial.println("Sistema avviato. Buzzer abilitato per futuri allarmi.");

  server.sendHeader("Location", "/dashboard");
  server.send(302);
}

void handleStop() {
  Serial.println("Sistema arrestato (web).");
  systemActive = false;
  setLed(OFF);
  buzzerOff();
  externalLedOff();
  lcdOff();

  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSilence() {
  if (systemActive) {
    buzzerSilenced = true;
    buzzerOff();
    Serial.println("Buzzer silenziato (web).");
  }
  server.sendHeader("Location", "/dashboard");
  server.send(302);
}

void handleUnsilence() {
  if (systemActive) {
    buzzerSilenced = false;
    Serial.println("Buzzer riattivato (web).");
    if (alarmActive) {
      buzzerOn();
    } else {
      buzzerOff();
    }
  }
  server.sendHeader("Location", "/dashboard");
  server.send(302);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("==========================");
  Serial.println("Terrario Monitor");
  Serial.println("==========================");

  pinMode(LED_R,  OUTPUT);
  pinMode(LED_G,  OUTPUT);
  pinMode(LED_B,  OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(LED_EXTERNAL, OUTPUT);

  buzzerOff();
  externalLedOff();

  digitalWrite(LED_R,  LOW);
  digitalWrite(LED_G,  LOW);
  digitalWrite(LED_B,  LOW);

  Serial.print("LCD...   ");
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Accensione...");
  Serial.println("OK");

  Serial.print("DHT...   ");
  dht.begin();
  Serial.println("OK");

  connectWiFi();

  Serial.print("Server...");
  server.on("/", handleRoot);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/dashboard", []() { server.send(200, "text/html", dashboard()); });
  server.on("/stop", handleStop);
  server.on("/silence", handleSilence);
  server.on("/unsilence", handleUnsilence);
  server.begin();
  Serial.println("OK");

  lcdOff();
  Serial.println("Sistema pronto. In attesa di avvio da web...");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  server.handleClient();

  if (!systemActive) return;

  // ----------------------------------------------------------
  // LETTURA PERIODICA
  // ----------------------------------------------------------
  static unsigned long lastRead = 0;
  if (millis() - lastRead < READ_INTERVAL) return;
  lastRead = millis();

  Serial.println("=========== LETTURA ===========");

  float t, h;
  bool dhtOk = readDHT(t, h);

  if (!dhtOk) {
    Serial.println("DHT: errore, uso ultimi valori.");
    t = lastT;
    h = lastH;
  } else {
    Serial.print("Temp: "); Serial.print(t, 1); Serial.println(" C");
    Serial.print("Hum:  "); Serial.print(h, 1); Serial.println(" %");
  }

  int l = analogRead(PHOTO);
  int rssi = WiFi.RSSI();
  Serial.print("Luce: "); Serial.println(l);
  Serial.print("RSSI: "); Serial.print(rssi); Serial.println(" dBm");

  // ----------------------------------------------------------
  // LOGICA LED + BUZZER
  // ----------------------------------------------------------
  alarmActive = false;

  LedColor c = OFF;

  if (!dhtOk) {
    c = RED;
    setLed(RED);
    alarmActive = true;
  }
  else if (t >= TEMP_MAX || h >= HUM_MAX) {
    c = RED;
    setLed(RED);
    alarmActive = true;
  }
  else if (t <= TEMP_MIN) {
    c = BLUE;
    setLed(BLUE);
    alarmActive = true;
  }
  else if (h <= HUM_MIN) {
    c = YELLOW;
    setLed(YELLOW);
    alarmActive = true;
  }
  else if (l <= LIGHT_MIN) {
    c = PURPLE;
    setLed(PURPLE);
  }
  else if (rssi < (int)WIFI_MIN_RSSI) {
    externalLedOn();
    c = GREEN;
    setLed(GREEN);
  }
  else {
    c = GREEN;
    setLed(GREEN);
    externalLedOff();
  }

  // Gestione buzzer
  if (alarmActive) {
    buzzerOn();
  } else {
    buzzerSilenced = false; 
    buzzerOff();
  }

  Serial.print("LED:  "); Serial.println(ledColorName(c));
  Serial.print("Buzzer silenziato: "); Serial.println(buzzerSilenced ? "SI" : "NO");
  Serial.println("================================");

  lcdUpdate(t, h, l);
  writeDB(t, h, l, rssi, (int)c);

  lastT    = t;
  lastH    = h;
  lastL    = l;
  lastRssi = rssi;
}

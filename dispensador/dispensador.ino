// Dispensador manual: WeMos D1 R1 + LCD Keypad Shield.
// Protocolo: PROTOCOLO.md de la central (sección Dispenser).
//   RIGHT          -> cargado      (finished)
//   LEFT           -> sin stock    (error EMPTY)
//   UP 3 segundos  -> portal WiFi  (solo sin pedido)
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <LittleFS.h>
#include <WiFiManager.h>

// La central cancela el pedido si no recibe "finished" en DispenserFinishTimeoutSec (60 s).
const unsigned long ORDER_TIMEOUT_MS = 60000;
const unsigned long WIFI_HOLD_MS = 3000;
const unsigned long PORTAL_TIMEOUT_S = 180;

struct Config {
  char host[65] = "";
  char port[6] = "1883";
  char user[33] = "admin_admin";
  char pass[65] = "admin";
  char id[33] = "DSP-01";
} cfg;

// Pedido en pantalla: orderRequest vacío significa que no hay pedido.
String orderRequest, orderName;
unsigned long orderSince = 0;
// Último resultado: se reenvía si la central repite el mismo start.
String lastRequest, lastCode;
bool lastSent = true;
const char* note = "";

LiquidCrystal lcd(D8, D9, D4, D5, D6, D7);
WiFiClient network;
PubSubClient mqtt(network);
String cmdTopic, statusTopic, eventTopic, telemetryTopic;
const char* broadcastTopic = "devices/broadcast/telemetry_request";
unsigned long lastConnectAttempt = 0;

// Los botones del shield comparten A0 mediante un divisor de resistencias.
// Medido en esta placa: RIGHT 13, UP 177, DOWN 432, LEFT 674, suelto y SELECT 1024.
// Cada umbral está a mitad de camino entre dos botones vecinos.
// El monitor serie imprime A0 en cada cambio para comprobarlo.
enum class Key { None, Accept, Wifi, Cancel };
Key keyFromA0(int value) {
  if (value < 95) return Key::Accept;
  if (value < 305) return Key::Wifi;
  if (value < 553) return Key::None;
  if (value < 849) return Key::Cancel;
  return Key::None;
}

Key rawKey = Key::None, heldKey = Key::None;
unsigned long rawSince = 0, lastSample = 0, wifiSince = 0;

// Devuelve la tecla en el momento en que se pulsa, una sola vez por pulsación.
Key pressedKey() {
  // Leer A0 sin pausa desconecta el WiFi del ESP8266.
  if (millis() - lastSample < 20) return Key::None;
  lastSample = millis();
  int value = analogRead(A0);
  Key key = keyFromA0(value);
  if (key != rawKey) { rawKey = key; rawSince = millis(); Serial.printf("A0=%d\n", value); }
  if (rawKey == heldKey || millis() - rawSince < 50) return Key::None;
  heldKey = rawKey;
  return heldKey;
}

bool validConfig() {
  char* end = nullptr;
  long port = strtol(cfg.port, &end, 10);
  return cfg.host[0] && cfg.id[0] && end != cfg.port && *end == '\0' && port >= 1 && port <= 65535;
}

bool loadConfig() {
  File file = LittleFS.open("/mqtt.json", "r");
  if (!file) return false;
  JsonDocument doc;
  bool parsed = !deserializeJson(doc, file);
  file.close();
  if (!parsed) return false;
  strlcpy(cfg.host, doc["host"] | "", sizeof(cfg.host));
  strlcpy(cfg.port, doc["port"] | "", sizeof(cfg.port));
  strlcpy(cfg.user, doc["user"] | "", sizeof(cfg.user));
  strlcpy(cfg.pass, doc["password"] | "", sizeof(cfg.pass));
  strlcpy(cfg.id, doc["id"] | "", sizeof(cfg.id));
  return validConfig();
}

bool saveConfig() {
  JsonDocument doc;
  doc["host"] = cfg.host; doc["port"] = cfg.port; doc["user"] = cfg.user;
  doc["password"] = cfg.pass; doc["id"] = cfg.id;
  File file = LittleFS.open("/mqtt.json", "w");
  if (!file) return false;
  bool written = serializeJson(doc, file) > 0;
  file.close();
  return written;
}

void show(const char* line1, const char* line2) {
  lcd.clear(); lcd.print(line1); lcd.setCursor(0, 1); lcd.print(line2);
}

// Bloquea hasta que se guarda o vence, y después reinicia con la configuración nueva.
void openPortal() {
  char apName[17], apPass[17];
  snprintf(apName, sizeof(apName), "DSP-%06X", ESP.getChipId());
  snprintf(apPass, sizeof(apPass), "dsp%06X", ESP.getChipId());
  show(apName, apPass);
  WiFiManager portal;
  portal.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  WiFiManagerParameter host("host", "IP del servidor MQTT", cfg.host, 64);
  WiFiManagerParameter port("port", "Puerto MQTT", cfg.port, 5);
  WiFiManagerParameter user("user", "Usuario MQTT", cfg.user, 32);
  WiFiManagerParameter pass("pass", "Clave MQTT", cfg.pass, 64, "type='password'");
  WiFiManagerParameter id("id", "ID del dispensador", cfg.id, 32);
  portal.addParameter(&host); portal.addParameter(&port); portal.addParameter(&user);
  portal.addParameter(&pass); portal.addParameter(&id);
  bool connected = portal.startConfigPortal(apName, apPass);
  strlcpy(cfg.host, host.getValue(), sizeof(cfg.host));
  strlcpy(cfg.port, port.getValue(), sizeof(cfg.port));
  strlcpy(cfg.user, user.getValue(), sizeof(cfg.user));
  strlcpy(cfg.pass, pass.getValue(), sizeof(cfg.pass));
  strlcpy(cfg.id, id.getValue(), sizeof(cfg.id));
  if (!connected) show("WiFi no conecto", "Reiniciando");
  else if (!validConfig()) show("Datos invalidos", "Reiniciando");
  else if (!saveConfig()) show("Error memoria", "Reiniciando");
  else show("Guardado", "Reiniciando");
  delay(3000);
  ESP.restart();
}

bool publishEvent(const char* event, const String& requestId, const String& code) {
  JsonDocument doc;
  doc["event"] = event;
  doc["request_id"] = requestId;
  if (code.length()) doc["code"] = code;
  char payload[256];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  return mqtt.publish(eventTopic.c_str(), (const uint8_t*)payload, length, false);
}

bool publishResult() {
  return publishEvent(lastCode.length() ? "error" : "finished", lastRequest, lastCode);
}

void publishTelemetry() {
  JsonDocument doc;
  doc["dispenser_id"] = cfg.id;
  doc["busy"] = orderRequest.length() > 0;
  doc["current_request_id"] = orderRequest;
  doc["last_finished_request_id"] = lastCode.length() ? String() : lastRequest;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime_s"] = millis() / 1000;
  doc["free_heap"] = ESP.getFreeHeap();
  char payload[320];
  size_t length = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(telemetryTopic.c_str(), (const uint8_t*)payload, length, false);
}

// Un start repetido nunca despacha dos veces: contesta lo mismo que antes.
void onStart(const String& requestId, const String& name) {
  if (requestId == orderRequest) { publishEvent("started", requestId, ""); return; }
  if (requestId == lastRequest) { publishResult(); return; }
  if (orderRequest.length()) { publishEvent("error", requestId, "BUSY"); return; }
  orderRequest = requestId;
  orderName = name.length() ? name : requestId;
  orderSince = millis();
  publishEvent("started", requestId, "");
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  const char* cmd = doc["cmd"] | "";
  if (!strcmp(cmd, "telemetry_request")) { publishTelemetry(); return; }
  String requestId = doc["request_id"] | "";
  if (cmdTopic == topic && !strcmp(cmd, "start") && requestId.length()) onStart(requestId, doc["order_id"] | "");
}

void connectMqtt() {
  String clientId = String("dispenser_") + cfg.id;
  if (!mqtt.connect(clientId.c_str(), cfg.user, cfg.pass, statusTopic.c_str(), 1, false, "{\"online\":false}", true)) {
    Serial.printf("MQTT error %d con %s:%s\n", mqtt.state(), cfg.host, cfg.port);
    return;
  }
  mqtt.subscribe(cmdTopic.c_str(), 1);
  mqtt.subscribe(broadcastTopic, 0);
  mqtt.publish(statusTopic.c_str(), "{\"online\":true}");
}

// La pantalla se deriva del estado en cada vuelta y solo se redibuja si cambió.
void drawScreen() {
  char line1[17], line2[17];
  if (orderRequest.length()) {
    snprintf(line1, sizeof(line1), "%s", orderName.c_str());
    snprintf(line2, sizeof(line2), "RIGHT=OK LEFT=NO");
  } else {
    const char* link = WiFi.status() != WL_CONNECTED ? "Sin WiFi" : mqtt.connected() ? "Esperando orden" : "Sin servidor";
    const char* hint = wifiSince ? "Abriendo WiFi..." : !lastSent ? "Enviando..." : note[0] ? note : "UP 3s = WiFi";
    snprintf(line1, sizeof(line1), "%s", link);
    snprintf(line2, sizeof(line2), "%s", hint);
  }
  static char shown1[17] = "\n", shown2[17] = "";
  if (!strcmp(line1, shown1) && !strcmp(line2, shown2)) return;
  strcpy(shown1, line1); strcpy(shown2, line2);
  show(line1, line2);
}

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);
  if (!LittleFS.begin()) { show("Error memoria", ""); while (true) delay(1000); }
  if (!loadConfig() || WiFi.SSID().length() == 0) openPortal();
  cmdTopic = String("devices/") + cfg.id + "/dispenser/cmd";
  statusTopic = String("devices/") + cfg.id + "/dispenser/status";
  eventTopic = String("devices/") + cfg.id + "/dispenser/event";
  telemetryTopic = String("devices/") + cfg.id + "/dispenser/telemetry";
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin();
  mqtt.setServer(cfg.host, atoi(cfg.port));
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(10);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected() && millis() - lastConnectAttempt >= 5000) {
    lastConnectAttempt = millis();
    connectMqtt();
  }
  if (mqtt.connected()) mqtt.loop();
  // El resultado se guarda aunque no haya conexión y se manda al reconectar.
  if (!lastSent && mqtt.connected()) lastSent = publishResult();

  if (orderRequest.length() && millis() - orderSince >= ORDER_TIMEOUT_MS) {
    orderRequest = "";
    note = "Ult: VENCIDO";
  }

  Key key = pressedKey();
  if (orderRequest.length() && (key == Key::Accept || key == Key::Cancel)) {
    lastRequest = orderRequest;
    lastCode = key == Key::Accept ? "" : "EMPTY";
    lastSent = false;
    orderRequest = "";
    note = key == Key::Accept ? "Ult: CARGADO" : "Ult: SIN STOCK";
  }

  // El portal reinicia la placa: solo se permite sin pedido ni resultado por enviar.
  bool canOpenPortal = heldKey == Key::Wifi && orderRequest.length() == 0 && lastSent;
  if (!canOpenPortal) wifiSince = 0;
  else if (!wifiSince) wifiSince = millis();
  else if (millis() - wifiSince >= WIFI_HOLD_MS) openPortal();

  drawScreen();
}

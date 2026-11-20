#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <SNMP_Agent.h>
#include <ESPAsyncWebServer.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_idf_version.h>

// ======================= CONFIGURAÇÕES =======================

// Wi-Fi
const char* ssid     = "SSID_IOT";
const char* password = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// IP estático
IPAddress local_IP(192, 168, 10, 252);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0); 
IPAddress primaryDNS(8, 8, 8, 8);

// Acesso ao endpoint /reboot (TROQUE!)
const char* ADMIN_USER = "nome_de_usuario";
const char* ADMIN_PASS = "senha_de_reset";

// I2C
#define SDA_PIN 21
#define SCL_PIN 22
#define I2C_SENSOR_ADDR 0x40

// Proteções
const uint32_t WDT_TIMEOUT_S        = 30;      // loop() travado por 30 s => reinicia
const uint32_t WIFI_DOWN_REBOOT_MS  = 30000;   // Wi-Fi fora por 30 s => reinicia
const uint32_t WIFI_BOOT_TIMEOUT_MS = 60000;   // sem Wi-Fi no boot por 60 s => reinicia
const uint32_t SENSOR_FAIL_RECOVER  = 5;       // a cada 5 falhas seguidas, tenta recuperar o barramento I2C
const uint32_t SENSOR_FAIL_REBOOT   = 150;     // ~5 min de falhas seguidas => reinicia
const uint32_t SCHEDULED_REBOOT_MS  = 0;       // reinício programado (ms de uptime). 0 = desativado

// SNMP
SNMPAgent snmp("empresax");
WiFiUDP udp;
const char* oidTemp = ".1.3.6.1.4.1.32473.1.1.0"; // o PEN 32473 é um número privado voltado somente para teste
const char* oidHum  = ".1.3.6.1.4.1.32473.1.2.0"; // o PEN 32473 é um número privado voltado somente para teste

AsyncWebServer server(80);

// ======================= ESTADO =======================

int intTemperature = 0;
int intHumidity = 0;
volatile float currentTemp = 0.0;
volatile float currentHum = 0.0;

unsigned long lastSensorMs = 0;
unsigned long lastWifiCheck = 0;
unsigned long wifiDisconnectTime = 0;
unsigned long lastHeapLog = 0;
volatile unsigned long lastLoopMs = 0;   // permite ao /status ver se o loop() está vivo
bool isDisconnected = false;
uint32_t sensorFails = 0;
uint32_t i2cRecoveries = 0;

// ======================= UTILITÁRIOS =======================

const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software (ESP.restart)";
    case ESP_RST_PANIC:    return "PANIC/crash";
    case ESP_RST_INT_WDT:  return "watchdog de interrupcao";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (alimentacao)";
    default:               return "outro";
  }
}

// Reinicia a placa por timer do sistema: funciona mesmo se o loop() estiver travado
void scheduleReboot(uint32_t delayMs) {
  static esp_timer_handle_t timer = nullptr;
  if (timer) return;  // já agendado
  esp_timer_create_args_t args = {};
  args.callback = [](void*) { ESP.restart(); };
  args.name = "reboot";
  if (esp_timer_create(&args, &timer) == ESP_OK) {
    esp_timer_start_once(timer, (uint64_t)delayMs * 1000ULL);
  }
}

void iniciarWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
  // Arduino-ESP32 core 3.x: o TWDT já vem inicializado, então só reconfigura
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  // core 2.x
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);  // monitora a task do loop()
}

// ======================= I2C =======================

void i2cIniciar() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(50);  // ms: evita ficar bloqueado no barramento
}

// Libera um barramento travado (escravo segurando SDA em baixo)
void i2cRecuperar() {
  Serial.println("Recuperando barramento I2C...");
  i2cRecoveries++;
  Wire.end();

  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT);
  for (int i = 0; i < 9; i++) {           // 9 pulsos de clock liberam o escravo
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(10);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  }
  // condição de STOP
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, LOW);  delayMicroseconds(10);
  digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(SDA_PIN, HIGH); delayMicroseconds(10);

  i2cIniciar();
}

// Lê um valor bruto de 16 bits (modo no-hold). Sensor tipo HTU21D / Si7021.
bool lerBruto(uint8_t cmd, uint16_t &raw) {
  Wire.beginTransmission(I2C_SENSOR_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;

  delay(60);  // conversão de temperatura 14 bits pode levar até ~50 ms

  if (Wire.requestFrom((uint8_t)I2C_SENSOR_ADDR, (uint8_t)2) != 2) return false;
  uint8_t msb = Wire.read();   // leituras separadas: ordem garantida
  uint8_t lsb = Wire.read();
  raw = (((uint16_t)msb << 8) | lsb) & 0xFFFC;  // zera os 2 bits de status
  return true;
}

bool readI2CSensor(float &temp, float &hum) {
  uint16_t rawHum, rawTemp;
  if (!lerBruto(0xF5, rawHum))  return false;
  if (!lerBruto(0xF3, rawTemp)) return false;

  hum  = ((125.0f * (float)rawHum) / 65536.0f) - 6.0f;
  temp = ((175.72f * (float)rawTemp) / 65536.0f) - 46.85f;
  return true;
}

// ======================= PÁGINA WEB =======================

String processarHTML() {
  String html;
  html.reserve(1600);  // evita realocações e fragmentação do heap
  html += "<!DOCTYPE html><html>";
  html += "<head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<meta http-equiv=\"refresh\" content=\"5\">";
  html += "<style>html { font-family: Arial, sans-serif; display: inline-block; margin: 0px auto; text-align: center; background-color: #f4f7f6; color: #333;}";
  html += ".container { margin-top: 50px; }";
  html += ".card { background-color: white; border-radius: 8px; padding: 20px; margin: 15px auto; width: 300px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += ".temp { color: #d9534f; font-size: 2.5rem; font-weight: bold; }";
  html += ".hum { color: #5bc0de; font-size: 2.5rem; font-weight: bold; }</style></head>";

  html += "<body><div class=\"container\">";
  html += "<h2>ESP32 - Monitor de Ambiente</h2>";
  html += "<p>Integrado com Zabbix via SNMP</p>";

  html += "<div class=\"card\"><h3>Temperatura</h3>";
  html += "<p class=\"temp\">" + String((float)currentTemp, 1) + " &deg;C</p></div>";

  html += "<div class=\"card\"><h3>Umidade</h3>";
  html += "<p class=\"hum\">" + String((float)currentHum, 1) + " %</p></div>";

  html += "</div></body></html>";
  return html;
}

// ======================= WI-FI =======================

void verificarConexao() {
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Aviso: Wi-Fi desconectado!");

      if (!isDisconnected) {
        wifiDisconnectTime = millis();
        isDisconnected = true;
        WiFi.begin(ssid, password);
      }

      if (millis() - wifiDisconnectTime > WIFI_DOWN_REBOOT_MS) {
        Serial.println("Forcando reinicio do ESP32 por falta de rede...");
        ESP.restart();
      }
    } else {
      isDisconnected = false;
    }
  }
}

// ======================= SETUP =======================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nBoot. Motivo do ultimo reset: %s\n", resetReasonStr());

  iniciarWatchdog();
  i2cIniciar();

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // desliga power save: evita "conectado mas sem resposta"

  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    Serial.printf("Wi-Fi desconectou, motivo=%d\n", info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Falha ao configurar o IP Estatico!");
  }

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi com IP estatico");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
    if (millis() - t0 > WIFI_BOOT_TIMEOUT_MS) {
      Serial.println("\nSem Wi-Fi no boot, reiniciando...");
      ESP.restart();
    }
  }

  Serial.println("\nWiFi Conectado com sucesso!");
  Serial.print("Painel Web disponivel em: http://");
  Serial.println(WiFi.localIP());

  // ---- Servidor web ----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", processarHTML());
  });

  // Diagnóstico: se loop_age_ms crescer sem parar, o loop() travou
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[300];
    snprintf(buf, sizeof(buf),
      "{\"uptime_s\":%lu,\"loop_age_ms\":%lu,\"heap_livre\":%u,\"heap_min\":%u,"
      "\"rssi\":%d,\"ultimo_reset\":\"%s\",\"falhas_sensor\":%lu,\"recuperacoes_i2c\":%lu}",
      (unsigned long)(millis() / 1000),
      (unsigned long)(millis() - lastLoopMs),
      (unsigned)ESP.getFreeHeap(),
      (unsigned)ESP.getMinFreeHeap(),
      WiFi.RSSI(),
      resetReasonStr(),
      (unsigned long)sensorFails,
      (unsigned long)i2cRecoveries);
    request->send(200, "application/json", buf);
  });

  // Reinício remoto: curl -u admin:senha http://10.100.10.252/reboot
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->authenticate(ADMIN_USER, ADMIN_PASS)) {
      request->requestAuthentication();
      return;
    }
    request->send(200, "text/plain", "Reiniciando em 1 segundo...");
    scheduleReboot(1000);  // não depende do loop()
  });

  server.begin();
  Serial.println("Servidor Web Assincrono iniciado!");

  // ---- SNMP ----
  snmp.setUDP(&udp);
  snmp.begin();
  snmp.addIntegerHandler(oidTemp, &intTemperature);
  snmp.addIntegerHandler(oidHum, &intHumidity);
  snmp.sortHandlers();  // necessário para snmpwalk/GETNEXT
}

// ======================= LOOP =======================

void loop() {
  esp_task_wdt_reset();     // só chega aqui se o loop() não travou
  lastLoopMs = millis();

  verificarConexao();
  snmp.loop();

  // Leitura do sensor a cada 2 s
  if (millis() - lastSensorMs >= 2000) {
    lastSensorMs = millis();

    float t, h;
    if (readI2CSensor(t, h)) {
      sensorFails = 0;
      currentTemp = t;
      currentHum = h;
      intTemperature = (int32_t)(t * 10);
      intHumidity = (int32_t)(h * 10);
    } else {
      sensorFails++;
      Serial.printf("Erro ao ler o sensor I2C (%lu seguidas)\n", (unsigned long)sensorFails);

      if (sensorFails % SENSOR_FAIL_RECOVER == 0) i2cRecuperar();

      if (sensorFails >= SENSOR_FAIL_REBOOT) {
        Serial.println("Sensor sem resposta por muito tempo, reiniciando...");
        ESP.restart();
      }
    }
  }

  // Log de saúde a cada 60 s (acompanhe o heap: se cair sempre, há vazamento)
  if (millis() - lastHeapLog >= 60000) {
    lastHeapLog = millis();
    Serial.printf("[saude] uptime=%lus heap=%u min=%u rssi=%d\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(),
                  WiFi.RSSI());
  }

  // Reinício programado opcional
  if (SCHEDULED_REBOOT_MS > 0 && millis() > SCHEDULED_REBOOT_MS) {
    Serial.println("Reinicio programado.");
    ESP.restart();
  }
}

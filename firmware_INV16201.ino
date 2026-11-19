#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>  // biblioteca controladora do I2C
#include <SNMP_Agent.h>
#include <ESPAsyncWebServer.h> // biblioteca para servidor de web assíncrono

// Configurações do Wi-Fi
const char* ssid = "SSID_IOT";
const char* password = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// Configuração de IP Estático (Sem DHCP)
IPAddress local_IP(192, 168, 10, 252);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0); 
IPAddress primaryDNS(8, 8, 8, 8);   // DNS do Google

// Definição dos pinos I2C que você deseja usar no ESP32 e o endereço do sensor
#define SDA_PIN 21
#define SCL_PIN 22
#define I2C_SENSOR_ADDR 0x40

// Inicializa o Servidor Web Assíncrono na porta 80
AsyncWebServer server(80);

// Configurações do SNMP
SNMPAgent snmp("empresa_x"); 
WiFiUDP udp;

int intTemperature = 0;
int intHumidity = 0;
float currentTemp = 0.0;
float currentHum = 0.0;

const char* oidTemp = ".1.3.6.1.4.1.32473.1.1.0";
const char* oidHum  = ".1.3.6.1.4.1.32473.1.2.0";

unsigned long lastMillis = 0;
unsigned long lastWifiCheck = 0;
unsigned long wifiDisconnectTime = 0;
bool isDisconnected = false;

// Função para ler o sensor I2C (0x40)
bool readI2CSensor(float &temp, float &hum) {
  Wire.beginTransmission(I2C_SENSOR_ADDR);
  Wire.write(0xF5); 
  if (Wire.endTransmission() != 0) return false;
  
  delay(20); 
  Wire.requestFrom((uint8_t)I2C_SENSOR_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return false;
  unsigned int rawHum = Wire.read() << 8 | Wire.read();

  Wire.beginTransmission(I2C_SENSOR_ADDR);
  Wire.write(0xF3); 
  if (Wire.endTransmission() != 0) return false;
  
  delay(20); 
  Wire.requestFrom((uint8_t)I2C_SENSOR_ADDR, (uint8_t)2);
  if (Wire.available() < 2) return false;
  unsigned int rawTemp = Wire.read() << 8 | Wire.read();

  hum = ((125.0 * (float)rawHum) / 65536.0) - 6.0;
  temp = ((175.72 * (float)rawTemp) / 65536.0) - 46.85;

  return true;
}

// Função para gerar dinamicamente a página HTML com os valores atuais do sensor
String processarHTML() {
  String html = "<!DOCTYPE html><html>";
  html += "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<meta http-equiv=\"refresh\" content=\"5\">"; // Autorefresh de 5 segundos
  html += "<style>html { font-family: Arial, sans-serif; display: inline-block; margin: 0px auto; text-align: center; background-color: #f4f7f6; color: #333;}";
  html += ".container { margin-top: 50px; }";
  html += ".card { background-color: white; border-radius: 8px; padding: 20px; margin: 15px auto; width: 300px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }";
  html += ".temp { color: #d9534f; font-size: 2.5rem; font-weight: bold; }";
  html += ".hum { color: #5bc0de; font-size: 2.5rem; font-weight: bold; }</style></head>";
  
  html += "<body><div class=\"container\">";
  html += "<h2>ESP32 - Monitor de Ambiente</h2>";
  html += "<p>Integrado com Zabbix via SNMP (Hardware I2C Ass&iacute;ncrono)</p>";
  
  // Card de Temperatura
  html += "<div class=\"card\">";
  html += "<h3>Temperatura</h3>";
  html += "<p class=\"temp\">" + String(currentTemp, 1) + " &deg;C</p>";
  html += "</div>";
  
  // Card de Umidade
  html += "<div class=\"card\">";
  html += "<h3>Umidade</h3>";
  html += "<p class=\"hum\">" + String(currentHum, 1) + " %</p>";
  html += "</div>";
  
  html += "</div></body></html>";
  return html;
}

// Função para monitorar quedas de Wi-Fi e forçar reboot se necessário
void verificarConexao() {
  if (millis() - lastWifiCheck > 5000) { // Verifica a rede a cada 5 segundos
    lastWifiCheck = millis();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Aviso: Wi-Fi desconectado!");
      
      if (!isDisconnected) {
        wifiDisconnectTime = millis();
        isDisconnected = true;
        WiFi.begin(ssid, password); // Tenta reconectar em background
      }
      
      // Se passar mais de 30 segundos desconectado, reinicia a placa por completo
      if (millis() - wifiDisconnectTime > 30000) {
        Serial.println("Forcando reinicio do ESP32 por falta de rede...");
        ESP.restart();
      }
    } else {
      isDisconnected = false;
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Inicializa o barramento I2C
  Wire.begin(SDA_PIN, SCL_PIN);

  // Aplica o IP estático antes do Wi-Fi
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Falha ao configurar o IP Estatico!");
  }

  // Conexão Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi com IP estatico");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Conectado com sucesso!");
  Serial.print("Painel Web disponivel em: http://");
  Serial.println(WiFi.localIP());
  
  // CONFIGURAÇÃO DO SERVIDOR WEB ASSÍNCRONO
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    // Responde instantaneamente enviando o HTML atualizado
    request->send(200, "text/html", processarHTML());
  });

  // Inicializa o servidor Web em segundo plano
  server.begin();
  Serial.println("Servidor Web Assincrono iniciado!");

  // Inicia o Agente SNMP
  snmp.setUDP(&udp);
  snmp.begin();
  snmp.addIntegerHandler(oidTemp, &intTemperature);
  snmp.addIntegerHandler(oidHum, &intHumidity);
}

void loop() {
  // Mantém a saúde da conexão Wi-Fi ativa
  verificarConexao();

  // Processa requisições SNMP do Zabbix
  snmp.loop(); 

  // Atualiza a leitura do sensor a cada 2 segundos
  if (millis() - lastMillis > 2000) {
    lastMillis = millis();

    float t, h;
    if (readI2CSensor(t, h)) {
      currentTemp = t;
      currentHum = h;
      intTemperature = (int32_t)(t * 10); // Multiplicado por 10 para o Zabbix
      intHumidity = (int32_t)(h * 10);    // Multiplicado por 10 para o Zabbix
    } else {
      Serial.println("Erro ao ler o sensor I2C!");
    }
  }
}

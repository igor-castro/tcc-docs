#include <WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>
#include <SNMP_Agent.h>

// Configurações do Wi-Fi
const char* ssid = "SSID_IOT";
const char* password = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

// Configuração de IP Estático (Sem DHCP)
IPAddress local_IP(192, 168, 10, 252);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0); 
IPAddress primaryDNS(8, 8, 8, 8);   // DNS do Google

// Configurações do DHT22
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Servidor Web na porta 80
WiFiServer server(80);

// Configurações do SNMP
SNMPAgent snmp("empresa_x"); 
WiFiUDP udp;

int intTemperature = 0;
int intHumidity = 0;
float currentTemp = 0.0;
float currentHum = 0.0;

const char* oidTemp = ".1.3.6.1.4.1.67727.1.1.0"; //OID da Temperatura
const char* oidHum  = ".1.3.6.1.4.1.67727.1.2.0"; //OID da Umidade

unsigned long lastMillis = 0;

void setup() {
  Serial.begin(115200);
  dht.begin();

  // Aplica a configuração de IP estático ANTES de conectar ao Wi-Fi
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Falha ao configurar o IP Estático!");
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
  
  // Inicia o servidor Web
  server.begin();

  // Inicia o Agente SNMP
  snmp.setUDP(&udp);
  snmp.begin();
  snmp.addIntegerHandler(oidTemp, &intTemperature);
  snmp.addIntegerHandler(oidHum, &intHumidity);
}

void loop() {
  // Processa requisições SNMP do Zabbix
  snmp.loop(); 

  // Atualiza a leitura do sensor a cada 2 segundos
  if (millis() - lastMillis > 2000) {
    lastMillis = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      currentTemp = t;
      currentHum = h;
      intTemperature = (int32_t)(t*10); // Para o Zabbix
      intHumidity = (int32_t)(h*10);    // Para o Zabbix
    }
  }

  // Processa requisições de usuários acessando a página HTML
  WiFiClient client = server.available();
  if (client) {
    Serial.println("Novo cliente conectado ao Servidor Web");
    String currentLine = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') {
          if (currentLine.length() == 0) {
            // Envia o cabeçalho de resposta HTTP padrão
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html; charset=utf-8");
            client.println("Connection: close");
            client.println();
            
            // Página HTML com autorefresh de 5 segundos
            client.println("<!DOCTYPE html><html>");
            client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("<meta http-equiv=\"refresh\" content=\"5\">"); 
            client.println("<style>html { font-family: Arial, sans-serif; display: inline-block; margin: 0px auto; text-align: center; background-color: #f4f7f6; color: #333;}");
            client.println(".container { margin-top: 50px; }");
            client.println(".card { background-color: white; border-radius: 8px; padding: 20px; margin: 15px auto; width: 300px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }");
            client.println(".temp { color: #d9534f; font-size: 2.5rem; font-weight: bold; }");
            client.println(".hum { color: #5bc0de; font-size: 2.5rem; font-weight: bold; }</style></head>");
            
            client.println("<body><div class=\"container\">");
            client.println("<h2>ESP32 - Monitor de Ambiente</h2>");
            client.println("<p>Integrado com Zabbix via SNMP (IP Fixo)</p>");
            
            // Card de Temperatura
            client.println("<div class=\"card\">");
            client.println("<h3>Temperatura</h3>");
            client.print("<p class=\"temp\">"); client.print(currentTemp, 1); client.println(" &deg;C</p>");
            client.println("</div>");
            
            // Card de Umidade
            client.println("<div class=\"card\">");
            client.println("<h3>Umidade</h3>");
            client.print("<p class=\"hum\">"); client.print(currentHum, 1); client.println(" %</p>");
            client.println("</div>");
            
            client.println("</div></body></html>");
            client.println();
            break;
          } else {
            currentLine = "";
          }
        } else if (c != '\r') {
          currentLine += c;
        }
      }
    }
    client.stop();
    Serial.println("Cliente desconectado.");
  }
}

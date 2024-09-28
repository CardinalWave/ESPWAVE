#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <uri/UriGlob.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include <WebSocketsServer.h>

#include <Preferences.h>

#include <ArduinoJson.h>
#include <map>
#include <string>

#ifndef APSSID
#define APSSID "CardinalCloudAP"
#define APPSK "password"
#endif

const char *softAP_ssid = APSSID;
const char *softAP_password = APPSK;

const char *myHostname = "cardinalwave";

char ssid[33] = "CardinalCloud";
char password[65] = "serverPassword";

HTTPClient http;

// MQTT
const char *MQTT_HOST = "192.168.12.1";
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "ESP8266_001";
const char *MQTT_USER = "ESP8266_01";
const char *MQTT_PASSWORD = "";
const char *TOPIC = "esp8266_01";
const char *TOPIC_SERVER = "server";
// static const char *topic[] = {send, receive}; 

#define BEGIN_OF_PAD 0

WiFiClient client;
PubSubClient mqttClient(client);

// DNS server
const byte DNS_PORT = 53;
DNSServer dnsServer;

// Web server
ESP8266WebServer server(80);

// WebSocket
WebSocketsServer webSocket = WebSocketsServer(5000);

/* Soft AP network parameters */
IPAddress apIP(172, 217, 28, 1);
IPAddress netMsk(255, 255, 255, 0);


/** Should I connect to WLAN asap? */
boolean connect;

/** Last time I tried to connect to WLAN */
unsigned long lastConnectTry = 0;

/** Current WLAN status */
unsigned int status = WL_IDLE_STATUS;

// Preferences ( ESP - MemoryManagent)
Preferences preferences;

// Client WebSocket
struct WebSocketMQTTSessao {
  uint8_t webSocketClientID;
  char mqttTopic[100];
};

// Topic Type
enum TopicType {
  FULL_TOPIC,
  SESSION_TOPIC
};

WebSocketMQTTSessao sessions[10]; // Supondo um máximo de 10 sessões simultâneas ///////// FAZER O CW-BFF-SERVICE MANDAR A SESSAO E DISPOSITIVO QUANDO CONECTADO NO SOCKET
int sessionCount = 0;

void addSession(uint8_t webSocketClientID, const char* mqttTopic) {
  if (sessionCount < 10) {
    sessions[sessionCount].webSocketClientID = webSocketClientID;
    strncpy(sessions[sessionCount].mqttTopic, mqttTopic, sizeof(sessions[sessionCount].mqttTopic));
    sessionCount++;
    Serial.printf("WebSocket Client ID: %u\n", webSocketClientID);
    Serial.printf("MQTT Topic: %s\n", mqttTopic);
  } else {
    Serial.println("Número máximo de sessões alcançado");
  }
}

void removeSession(uint8_t webSocketClientID) {
  for (int i = 0; i < sessionCount; i++) {
    if (sessions[i].webSocketClientID == webSocketClientID) {
      for (int j = i; j < sessionCount - 1; j++) {
        sessions[j] = sessions[j + 1];
      }
      sessionCount--;
      break;
    }
  }
}

uint8_t getWebSocketId(const char* mqttTopic) {
  for (int i = 0; i < sessionCount; i++) {
    Serial.print(sessions[i].mqttTopic);
    Serial.print(sessions[i].webSocketClientID);
    Serial.println();
    if (sessions[i].mqttTopic == mqttTopic) {
      Serial.println(sessions[i].mqttTopic);
      return sessions[i].webSocketClientID;
      break;
    }
  }
  return 0;
}

char* formatTopic(const char* sessionId, const char* action, const char *device){
  size_t totalLength = strlen(device) + strlen(sessionId) + strlen(action) + 5;
  char* format = new char[totalLength];
  snprintf(format, totalLength, "/%s/%s/%s", device, sessionId, action);
  return format;
}

char* extractSessionTopic(const char* fullTopic) {
  const char* thirdSlash = strchr(strchr(strchr(fullTopic, '/') + 1, '/') + 1, '/');
  if (thirdSlash == nullptr) return nullptr;

  if (thirdSlash != nullptr) {
    size_t lenght = thirdSlash - fullTopic;
    char* sessionTopic = new char[lenght + 1];
    strncpy(sessionTopic, fullTopic, lenght);
    sessionTopic[lenght] = '\0';
    return sessionTopic;
  } else {
    return strdup(fullTopic);
  }
}

// JSON
const char **deserializationJson(const char* payload){
  // Use arduinojson.org/assistant to compute the capacity.
  StaticJsonDocument<200> doc;

  // Deserialize o JSON
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
      Serial.print("Falha ao deserializar JSON: ");
      Serial.println(error.c_str());
      return {};
  }
  const char* sessionId = doc["session_id"]; 
  const char* action = doc["action"];

  char* send = formatTopic(sessionId, action, TOPIC);
  char* receive = formatTopic(sessionId, action, TOPIC_SERVER);

  static const char *topic[] = {send, receive}; 
  return topic;
}
/** Is this an IP? */
boolean isIp(String str) {
  for (size_t i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) { return false; }
  }
  return true;
}

/** IP to String? */
String toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) { res += String((ip >> (8 * i)) & 0xFF) + "."; }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}

char* uint8_to_string(uint8_t valor_uint8) {
    char* str = (char*)malloc(2 * sizeof(char)); // Aloca memória para a string
    if (str == NULL) {
        // Tratamento de erro se a alocação de memória falhar
        fprintf(stderr, "Erro ao alocar memória\n");
        exit(EXIT_FAILURE);
    }

    // Convertendo uint8_t para string
    snprintf(str, 2, "%c", valor_uint8); // Usa snprintf para garantir que não haja estouro de buffer

    return str;
}

void messageWebSocket(char* payload, char* topic) {
  uint8_t webSocketId = getWebSocketId(topic);
  Serial.print("Send to WebSocket [");
  Serial.print(topic);
  Serial.print("|");
  Serial.print(webSocketId);
  Serial.print("] ");
  Serial.println();
  Serial.println(payload);
  webSocket.sendTXT(webSocketId, payload);
}

// MQTT
void callback(char* topic, byte* payload, unsigned int length)
{   
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");

    char* payloadStr = new char[length + 1];
    memcpy(payloadStr, payload, length);
    payloadStr[length] = '\0'; // Adiciona o terminador nulo

    Serial.println();
    Serial.println(payloadStr);
    messageWebSocket(payloadStr, topic);
}

void messageOverMqtt(const char **topics, const char *payload) {
    mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD );
    mqttClient.subscribe(topics[1]);
    mqttClient.publish(topics[0], payload);
    Serial.printf("Send message on %s", topics[0]);
    Serial.println();
}

// WebSocket
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      removeSession(num);
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      }
      break;
    case WStype_TEXT:
      const char **topics = deserializationJson((char*)payload);
      messageOverMqtt(topics, (char*)payload);
      addSession(num, extractSessionTopic(topics[0]));
      break;
  }
}

/** Redirect to captive portal if we got a request for another domain. Return true in that case so the page handler do not try to handle the request again. */
boolean captivePortal() {
  Serial.println(server.hostHeader());
  if (!isIp(server.hostHeader()) && server.hostHeader() != (String(myHostname) + ".net")) {
    Serial.println("Request redirected to captive portal");
    server.sendHeader("Location", String("http://") + (String(myHostname) + ".net"), true);
    server.send(302, "text/plain", "");  // Empty content inhibits Content-length header so we have to close the socket ourselves.
    server.client().stop();              // Stop is needed because we sent no content length
    return true;
  }
  return false;
}
/** Handle root or redirect to captive portal */
void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  http.begin(client, "http://cardinalwave.net");
  while (http.GET() <= 0) {
    http.begin(client, "http://cardinalwave.net"); 
  }
  String payload = http.getString();
  server.send(200, "text/html", payload);
}

void handleNotFound() {
  if (captivePortal()) {  // If caprive portal redirect instead of displaying the error page.
    return;
  }
  String message = F("File Not Found\n\n");
  message += F("URI: ");
  message += server.uri();
  message += F("\nMethod: ");
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += F("\nArguments: ");
  message += server.args();
  message += F("\n");

  for (uint8_t i = 0; i < server.args(); i++) { message += String(F(" ")) + server.argName(i) + F(": ") + server.arg(i) + F("\n"); }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(404, "text/plain", message);
}

void setup() {
  delay(1000);
  Serial.begin(9600);
  Serial.println();
  Serial.println("Configuring access point...");
  /* You can remove the password parameter if you want the AP to be open. */
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(softAP_ssid, softAP_password);
  delay(500);  // Without delay I've seen the IP address blank
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  /* Setup the DNS server redirecting all the domains to the apIP */
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);

  /* Setup WebSocket */
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  /* Setup web pages: root, wifi config pages, SO captive portal detectors and not found. */
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound); 
  server.begin();  // Web server start

  preferences.begin("cw-page", false); 
  preferences.clear();

  // JSON

  Serial.println("HTTP server started");
  // loadCredentials();           // Load WLAN credentials from network
  connect = strlen(ssid) > 0;  // Request WLAN connect if there is a SSID
}

void connectWifi() {
  Serial.println("Connecting as wifi client...");
  // WiFi.disconnect();
  WiFi.begin(ssid, password);
  int connRes = WiFi.waitForConnectResult();
  Serial.print("connRes: ");
  Serial.println(connRes);
}

void loop() {
  if (connect) {
    Serial.println("Connect requested");
    connect = false;
    connectWifi();
    lastConnectTry = millis();
  }
  {
    unsigned int s = WiFi.status();
    if (s == 0 && millis() > (lastConnectTry + 60000)) {
      connect = true;
    }
    if (status != s) {  // WLAN status change
      Serial.print("Status: ");
      Serial.println(s);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(5000);
      status = s;
      if (s == WL_CONNECTED) {
        /* Just connected to WLAN */
        Serial.println("");
        Serial.print("Connected to ");
        Serial.println(ssid);
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
    
        while (WiFi.status() != WL_CONNECTED) {
          delay(500);
          Serial.print(".");
        }

        Serial.println("Connected to Wi-Fi");
        mqttClient.setServer(MQTT_HOST, MQTT_PORT);
       
        while (!client.connected()) {
          if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD )) {
              Serial.println("Connected to MQTT broker");
              mqttClient.setCallback(callback);
              mqttClient.publish("/esp8266/device/connect", "ping");
              mqttClient.subscribe("/server/#");
              mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD );
          } else {
              delay(500);
              Serial.print(".");
          }
        }

        // Setup MDNS responder
        if (!MDNS.begin(myHostname)) {
          Serial.println("Error setting up MDNS responder!");
        } else {
          Serial.println("mDNS responder started");
          // Add service to MDNS-SD
          MDNS.addService("http", "tcp", 80);
        }
      } else if (s == WL_NO_SSID_AVAIL) {
        WiFi.disconnect();
      }
    }
    if (s == WL_CONNECTED) { MDNS.update();  }
  }
  // DNS
  dnsServer.processNextRequest();
  // MQTT
  mqttClient.loop();
  // HTTP
  server.handleClient();
  // WebSocket
  webSocket.loop();

  
}
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

#ifndef APSSID
#define APSSID "CardinalWaveAP"
#define APPSK "password"
#endif

const char *softAP_ssid = APSSID;
const char *softAP_password = APPSK;

const char *myHostname = "cardinalwave";

char ssid[33] = "CardinalWaveSV";
char password[65] = "serverPassword";

HTTPClient http;

// MQTT
const char *MQTT_HOST = "192.168.12.1";
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT_ID = "ESP8266_001";
const char *MQTT_USER = "ESP8266_01";
const char *MQTT_PASSWORD = "";
const char *TOPIC = "esp8266_01/logs";

WiFiClient client;
PubSubClient mqttClient(client);

// DNS server
const byte DNS_PORT = 53;
DNSServer dnsServer;

// Web server
ESP8266WebServer server(80);

// WebSocket
WebSocketsServer webSocket = WebSocketsServer(81);

/* Soft AP network parameters */
IPAddress apIP(172, 217, 28, 1);
IPAddress netMsk(255, 255, 255, 0);


/** Should I connect to WLAN asap? */
boolean connect;

/** Last time I tried to connect to WLAN */
unsigned long lastConnectTry = 0;

/** Current WLAN status */
unsigned int status = WL_IDLE_STATUS;


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

// MQTT
void callback(char* topic, byte* payload, unsigned int length)
{
    payload[length] = '\0';
    int value = String((char*) payload).toInt();

    Serial.println(topic);
    Serial.println(value);
   
}

// WebSocket
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] Received text: %s\n", num, payload);
      // Send a response back to the client
      String echoMessage = "Received:  " + String((char*)payload);
      webSocket.sendTXT(num, echoMessage);
      break;
  }
}

/** Redirect to captive portal if we got a request for another domain. Return true in that case so the page handler do not try to handle the request again. */
boolean captivePortal() {
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
  if (captivePortal()) {  // If caprive portal redirect instead of displaying the page.
    return;
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  String payloadReturn;
  http.begin(client, "http://cardinalwave.net");
  int httpCode = http.GET();
  Serial.print(httpCode);
  if(httpCode > 0 || httpCode == -1) {
    String payload = http.getString();
    Serial.println(payload);
    payloadReturn = payload;
    server.send(200, "text/html", payloadReturn);
  }  
  http.end();
  payloadReturn = "<h1>Not Found</h1>";
  server.send(200, "text/html", payloadReturn);
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
      /* If WLAN disconnected and idle try to connect */
      /* Don't set retry time too low as retry interfere the softAP operation */
      connect = true;
    }
    if (status != s) {  // WLAN status change
      Serial.print("Status: ");
      Serial.println(s);
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
          } else {
              delay(500);
              Serial.print(".");
          }
        }

        mqttClient.subscribe(TOPIC);
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
    if (s == WL_CONNECTED) { MDNS.update();  mqttClient.setCallback(callback);}
  }
  // DNS
  dnsServer.processNextRequest();
  // HTTP
  server.handleClient();

  // WebSocket
  webSocket.loop();
  webSocket.broadcastTXT("{mensagem: teste}");
}
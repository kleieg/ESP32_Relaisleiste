// Import required libraries
#if defined(ESP8266)
#include "LittleFS.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncTCP.h>
#define FileSytem LittleFS
#elif defined(ESP32)
#include <WiFi.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#define FileSytem SPIFFS
#else
#error "Unsupported board"
#endif

#include <MQTT.h>
#include <ESPAsyncWebServer.h>

#include <NTPClient.h>
#include <Arduino_JSON.h>
#include <AsyncElegantOTA.h>

#include "WLAN_Credentials.h"
#include "config.h"

// LED blink nicht im log-modus !!!
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
#define SERIALINIT Serial.begin(115200);
#else
#define SERIALINIT
#endif

// will be computed as "<HOSTNAME>_<MAC-ADDRESS>"
String Hostname;

int WiFi_reconnect = 0;

// for WiFi
WiFiClient myClient;
long lastReconnectAttempt = 0;

// for MQTT
MQTTClient client(256);
long Mqtt_lastSend = 0;
int Mqtt_reconnect = 0;

// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
long My_time = 0;

// Timers auxiliar variables
long now = millis();
char strtime[8];
long LEDblink = 0;
bool led = LOW;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Create a WebSocket object
AsyncWebSocket ws("/ws");

// end of definitions -----------------------------------------------------

// Initialize file sytem
void initFS()
{
  if (!FileSytem.begin())
  {
    Serial.println("An error has occurred while mounting file sytem");
  }
  Serial.println("File system mounted successfully");
}

// Initialize WiFi
void initWiFi()
{
  // dynamically determine hostname
  Hostname = HOSTNAME;
  Hostname += "_";
  Hostname += WiFi.macAddress();
  Hostname.replace(":", "");

  WiFi.mode(WIFI_STA);
  WiFi.hostname(Hostname);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println(".");
    delay(1000);
  }
  Serial.printf("%s", WiFi.localIP().toString().c_str());
}

String getOutputStates()
{
  JSONVar myArray;

  myArray["cards"][0]["c_text"] = String(Hostname) + "   /   " + String(VERSION);
  myArray["cards"][1]["c_text"] = String(WiFi.RSSI());
  myArray["cards"][2]["c_text"] = String(MQTT_INTERVAL) + "ms";
  myArray["cards"][3]["c_text"] = String(My_time);
  myArray["cards"][4]["c_text"] = "WiFi = " + String(WiFi_reconnect) + "   MQTT = " + String(Mqtt_reconnect);
  myArray["cards"][5]["c_text"] = " ";
  myArray["cards"][6]["c_text"] = " to reboot click ok";

  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    myArray["gpios"][i]["output"] = String(i);

    if (relayReset[i] == "Y")
    {
      myArray["gpios"][i]["state"] = String(digitalRead(outputGPIOs[i]));
    }
    else
    {
      myArray["gpios"][i]["state"] = String(!digitalRead(outputGPIOs[i]));
    }
  }
  String jsonString = JSON.stringify(myArray);
  return jsonString;
}

void notifyClients(String state)
{
  ws.textAll(state);
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    // according to AsyncWebServer documentation this is ok
    data[len] = 0;

    Serial.println("Data received: ");
    Serial.printf("%s\n", data);

    JSONVar json = JSON.parse((const char *)data);
    if (json == nullptr)
    {
      Serial.println("Request is not valid json, ignoring");
      return;
    }
    if (!json.hasOwnProperty("action"))
    {
      Serial.println("Request is not valid json, ignoring");
      return;
    }
    if (!strcmp(json["action"], "states"))
    {
      notifyClients(getOutputStates());
    }
    else if (!strcmp(json["action"], "reboot"))
    {
      Serial.println("Reset..");
      ESP.restart();
    }
    else if (!strcmp(json["action"], "relais"))
    {
      if (!json.hasOwnProperty("data"))
      {
        Serial.println("Relais request is missing data, ignoring");
        return;
      }
      if (!json["data"].hasOwnProperty("relais"))
      {
        Serial.println("Relais request is missing relais number, ignoring");
        return;
      }
      if (JSONVar::typeof_(json["data"]["relais"]) != "number")
      {
        Serial.println("Relais request contains invali relais number, ignoring");
        return;
      }
      int relais = json["data"]["relais"];
      if (relais < 0 || relais >= NUM_OUTPUTS)
      {
        Serial.println("Relais request contains invali relais number, ignoring");
        return;
      }
      
      digitalWrite(outputGPIOs[relais], !digitalRead(outputGPIOs[relais]));
      notifyClients(getOutputStates());
      Serial.println("switch Relais");

      if (relayReset[relais] == "Y")
      {
        relayResetStatus[relais] = 1;
      }
    }
  }

  Mqtt_lastSend = now - MQTT_INTERVAL - 10; // --> MQTT send !!
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    Serial.printf("WebSocket client #%u disconnected\n", client->id());
    break;
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void initWebSocket()
{
  ws.onEvent(onEvent);
#if defined CREDENTIALS_WEB_USER && defined CREDENTIALS_WEB_PASSWORD
  ws.setAuthentication(CREDENTIALS_WEB_USER, CREDENTIALS_WEB_PASSWORD);
#endif
  server.addHandler(&ws);
}

// reconnect to WiFi
void reconnect_wifi()
{
  Serial.printf("%s\n", "WiFi try reconnect");

  WiFi_reconnect = WiFi_reconnect + 1;
  
  WiFi.reconnect();
  delay(500);

  if (WiFi.status() == WL_CONNECTED)
  {
    // Once connected, publish an announcement...
    Serial.printf("%s\n", "WiFi reconnected");
  }
}

// This functions reconnects your ESP32 to your MQTT broker

void reconnect_mqtt()
{
  String willTopic = Hostname + "/LWT";
  String cmdTopic = Hostname + "/CMD/+";

  Serial.printf("%s\n", "MQTT try reconnect");

  Mqtt_reconnect = Mqtt_reconnect + 1;
  
#if defined CREDENTIALS_MQTT_USER && defined CREDENTIALS_MQTT_PASSWORD
  if (client.connect(Hostname.c_str(), CREDENTIALS_MQTT_USER, CREDENTIALS_MQTT_PASSWORD))
#else
  if (client.connect(Hostname.c_str()))
#endif
  {
    Serial.printf("%s\n", "MQTT connected");

    client.publish(willTopic.c_str(), "Online", true, 0);
  
    client.subscribe(cmdTopic.c_str());
  } else {
    Serial.printf("Failed to connect to broker; error: %d\n", client.lastError());
  }
}

// receive MQTT messages
void MQTT_callback(String topic, String message)
{

  Serial.printf("Message arrived on topic: %s; Data: %s\n", topic.c_str(), message.c_str());
 
  String relaisTopic = Hostname + "/CMD/Relais";
  String strTopic = String(topic);

  if (!strTopic.startsWith(relaisTopic) || strTopic.length() != relaisTopic.length() + 1)
  {
    Serial.printf("Invalid topic %d, %d", strTopic.length(), relaisTopic.length());
    return;
  }
  int relais = strTopic[strTopic.length() - 1] - '0';
  if(relais < 0 || relais >= NUM_OUTPUTS) {
    Serial.printf("Invalid relais %d", relais);
    return;
  }

  if (message == "true")
  {
    digitalWrite(outputGPIOs[relais], LOW);
  }
  else if (message == "false")
  {
    digitalWrite(outputGPIOs[relais], HIGH);
  }

  notifyClients(getOutputStates());

  Mqtt_lastSend = now - MQTT_INTERVAL - 10; // --> MQTT send !!
}

// initialize MQTT
void initMQTT() {
  String willTopic = Hostname + "/LWT";
  
  Serial.printf("setup MQTT\n");
  
  client.begin(myClient);
  client.setHost(CREDENTIALS_MQTT_BROKER, 1883);
  client.onMessage(MQTT_callback);
  client.setWill(willTopic.c_str(), "Offline", true, 0);
}

void MQTTsend()
{
  JSONVar mqtt_data, actuators;

  String mqtt_tag = Hostname + "/STATUS";
  Serial.printf("%s\n", mqtt_tag.c_str());

  char property[8];
  strcpy(property, "Relais0");

  for (size_t relais = 0; relais <= 6; relais++)
  {
    property[6] = '0' + relais;
    actuators[(const char*)property] = !digitalRead(outputGPIOs[relais]) ? true : false;
  }
  
  mqtt_data["Time"] = My_time;
  mqtt_data["RSSI"] = WiFi.RSSI();
  mqtt_data["Actuators"] = actuators;

  String mqtt_string = JSON.stringify(mqtt_data);

  Serial.printf("%s\n", mqtt_string.c_str());

  client.publish(mqtt_tag.c_str(), mqtt_string.c_str());

  notifyClients(getOutputStates());
}

String templateProcessor(const String& var)
{
  if(var == "NUM_OUTPUTS")
  {
    return String(NUM_OUTPUTS);
  }
  return "";
}

void setup()
{

  SERIALINIT

  Serial.println("init GPIOs\n");
  pinMode(GPIO_LED, OUTPUT);
  digitalWrite(GPIO_LED, LOW);

  // Set GPIOs as outputs. Set all relays to off when the program starts -  the relay is off when you set the relay to HIGH
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    pinMode(outputGPIOs[i], OUTPUT);
    digitalWrite(outputGPIOs[i], HIGH);
  }

  initFS();
  initWiFi();
  initWebSocket();
  initMQTT();

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(FileSytem, "/index.html", "text/html", false); });

  AsyncStaticWebHandler &handler = server.serveStatic("/", FileSytem, "/");
  #if defined CREDENTIALS_WEB_USER && defined CREDENTIALS_WEB_PASSWORD
    handler.setAuthentication(CREDENTIALS_WEB_USER, CREDENTIALS_WEB_PASSWORD);
  #endif
  handler.setTemplateProcessor(&templateProcessor);


  // init NTP
  Serial.printf("init NTP\n");
  timeClient.begin();
  timeClient.setTimeOffset(0);

  // Start ElegantOTA
  AsyncElegantOTA.begin(&server);

  // Start server
  server.begin();
}

void loop()
{
  ws.cleanupClients();

  now = millis();

  // update UPCtime
  timeClient.update();
  My_time = timeClient.getEpochTime();

  if (now - LEDblink > LED_BLINK_INTERVAL)
  {
    LEDblink = now;
    if (led == 0)
    {
      digitalWrite(GPIO_LED, HIGH);
      led = 1;
    }
    else
    {
      digitalWrite(GPIO_LED, LOW);
      led = 0;
    }
  }

  // auf Reset prüfen
  // falls nötig Timer setzten
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    if (relayResetStatus[i] == 1)
    {
      relayResetStatus[i] = 2;
      relayResetTimer[i] = now;
    }
  }

  // prüfen ob Timer abgelaufen; wenn ja Relais ausschalten
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    if (relayResetStatus[i] == 2)
    {
      if (now - relayResetTimer[i] > RELAY_RESET_INTERVAL)
      {
        relayResetStatus[i] = 0;
        digitalWrite(outputGPIOs[i], HIGH);
        notifyClients(getOutputStates());
        Mqtt_lastSend = now - MQTT_INTERVAL - 10; // --> MQTT send !!
      }
    }
  }

  // check WiFi
  if (WiFi.status() != WL_CONNECTED)
  {
    // try reconnect every 5 seconds
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL)
    {
      lastReconnectAttempt = now; // prevents mqtt reconnect running also
      // Attempt to reconnect
      Serial.println("WiFi reconnect");
      reconnect_wifi();
    }
  }
  else
  {
    // check if MQTT broker is still connected
    if (!client.connected())
    {
      // try reconnect every 5 seconds
      if (now - lastReconnectAttempt > RECONNECT_INTERVAL)
      {
        lastReconnectAttempt = now;
        // Attempt to reconnect
        Serial.println("MQTT reconnect");
        reconnect_mqtt();
      }
    }
    else
    {
      // Client connected

      client.loop();

      // send data to MQTT broker
      if (now - Mqtt_lastSend > MQTT_INTERVAL && now - lastReconnectAttempt > PUBLISH_DELAY)
      {
        Mqtt_lastSend = now;
        MQTTsend();
      }
    }
  }
}
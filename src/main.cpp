// Import required libraries
#include <WiFi.h>
#include <PubSubClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
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
PubSubClient client(myClient);
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

// Set number of outputs
#define NUM_OUTPUTS 7 // nur 7 ansprechbare Relais !!!

// Assign each GPIO to an output
int outputGPIOs[NUM_OUTPUTS] = {16, 17, 18, 19, 21, 22, 23};

// Assign relay details
String relayReset[NUM_OUTPUTS] = {"N", "N", "N", "N", "N", "N", "N"};
int relayResetStatus[NUM_OUTPUTS] = {0, 0, 0, 0, 0, 0, 0};
int relayResetTimer[NUM_OUTPUTS] = {0, 0, 0, 0, 0, 0, 0};

// end of definitions -----------------------------------------------------

// Initialize SPIFFS
void initSPIFFS()
{
  if (!SPIFFS.begin())
  {
    log_i("An error has occurred while mounting SPIFFS");
  }
  log_i("SPIFFS mounted successfully");
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
  log_i("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED)
  {
    log_i(".");
    delay(1000);
  }
  log_i("%s", WiFi.localIP().toString().c_str());
}

String getOutputStates()
{
  JSONVar myArray;

  myArray["cards"][0]["c_text"] = String(HOSTNAME) + "   /   " + String(VERSION);
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
      if (relais < 0 || relais > 7)
      {
        Serial.println("Relais request contains invali relais number, ignoring");
        return;
      }
      
      digitalWrite(outputGPIOs[relais], !digitalRead(outputGPIOs[relais]));
      notifyClients(getOutputStates());
      log_i("switch Relais");

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
    log_i("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    log_i("WebSocket client #%u disconnected\n", client->id());
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
  WiFi.begin();
  delay(500);
  if (WiFi.status() == WL_CONNECTED)
  {
    lastReconnectAttempt = 0;
    WiFi_reconnect = WiFi_reconnect + 1;
    // Once connected, publish an announcement...
    Serial.printf("%s\n", "WiFi reconnected");
  }
}

// This functions reconnects your ESP32 to your MQTT broker

void reconnect_mqtt()
{
  String willTopic = Hostname + "/LWT";
  String cmdTopic = Hostname + "/CMD";
#if defined CREDENTIALS_MQTT_USER && defined CREDENTIALS_MQTT_PASSWORD
  if (client.connect(Hostname.c_str(), CREDENTIALS_MQTT_USER, CREDENTIALS_MQTT_PASSWORD, willTopic.c_str(), 0, true, "Offline"))
#else
  if (client.connect(Hostname.c_str(), willTopic.c_str(), 0, true, "Offline"))
#endif
  {
    lastReconnectAttempt = 0;
    Serial.printf("%s\n", "connected");

    client.publish(willTopic.c_str(), "Online", true);

    client.subscribe(cmdTopic.c_str());

    Mqtt_reconnect = Mqtt_reconnect + 1;
  }
}

// receive MQTT messages
void MQTT_callback(char *topic, byte *message, unsigned int length)
{

  log_i("%s", "Message arrived on topic: ");
  log_i("%s\n", topic);
  log_i("%s", "Data : ");

  String MQTT_message;
  for (int i = 0; i < length; i++)
  {
    MQTT_message += (char)message[i];
  }
  log_i("%s\n", MQTT_message);

  String relaisTopic = Hostname + "/CMD/Relais";
  String strTopic = String(topic);

  if (!strTopic.startsWith(relaisTopic) || strTopic.length() != relaisTopic.length() + 1)
  {
    return;
  }
  int relais = strTopic[strTopic.length() - 1] - '0';

  if (MQTT_message == "on")
  {
    digitalWrite(outputGPIOs[relais], LOW);
  }
  else if (MQTT_message == "off")
  {
    digitalWrite(outputGPIOs[relais], HIGH);
  }

  notifyClients(getOutputStates());
}

void MQTTsend()
{
  JSONVar mqtt_data;

  String mqtt_tag = Hostname + "/SENSOR";
  log_i("%s\n", mqtt_tag.c_str());

  char property[8];
  strcpy(property, "Relais0");

  mqtt_data["Time"] = My_time;
  mqtt_data["RSSI"] = WiFi.RSSI();

  for (size_t relais = 0; relais <= 6; relais++)
  {
    property[6] = '0' + relais;
    mqtt_data[(const char*)property] = !digitalRead(outputGPIOs[relais]) ? true : false;
  }

  String mqtt_string = JSON.stringify(mqtt_data);

  log_i("%s\n", mqtt_string.c_str());

  client.publish(mqtt_tag.c_str(), mqtt_string.c_str());

  notifyClients(getOutputStates());
}

void setup()
{

  SERIALINIT

  log_i("init GPIOs\n");
  pinMode(GPIO_LED, OUTPUT);
  digitalWrite(GPIO_LED, LOW);

  // Set GPIOs as outputs. Set all relays to off when the program starts -  the relay is off when you set the relay to HIGH
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    pinMode(outputGPIOs[i], OUTPUT);
    digitalWrite(outputGPIOs[i], HIGH);
  }

  initSPIFFS();
  initWiFi();
  initWebSocket();

  log_i("setup MQTT\n");
  client.setServer(CREDENTIALS_MQTT_BROKER, 1883);
  client.setCallback(MQTT_callback);

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(SPIFFS, "/index.html", "text/html", false); });

  AsyncStaticWebHandler &handler = server.serveStatic("/", SPIFFS, "/");
  #if defined CREDENTIALS_WEB_USER && defined CREDENTIALS_WEB_PASSWORD
    handler.setAuthentication(CREDENTIALS_WEB_USER, CREDENTIALS_WEB_PASSWORD);
  #endif


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
      log_i("WiFi reconnect");
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
        log_i("MQTT reconnect");
        reconnect_mqtt();
      }
    }
    else
    {
      // Client connected

      client.loop();

      // send data to MQTT broker
      if (now - Mqtt_lastSend > MQTT_INTERVAL)
      {
        Mqtt_lastSend = now;
        MQTTsend();
      }
    }
  }
}
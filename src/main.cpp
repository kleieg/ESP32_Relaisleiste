// Import required libraries
#include <WiFi.h>
#include <PubSubClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <Arduino_JSON.h>
#include <AsyncElegantOTA.h>

#include "WLAN_Credentials.h"

// LED blink nicht im log-modus !!!
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
#define SERIALINIT Serial.begin(115200);
#else
#define SERIALINIT
#endif

//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<   Anpassungen !!!!
// set hostname used for MQTT tag and WiFi 
#define HOSTNAME "Relaisleiste"
#define VERSION "v 1.0.0"


// variables to connects to  MQTT broker
const char* mqtt_server = "192.168.178.15";
const char* willTopic = "tele/Relaisleiste/LWT";       // muss mit HOSTNAME passen !!!  tele/HOSTNAME/LWT    !!!

//<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<   Anpassungen Ende !!!!

int WiFi_reconnect = 0;

// for MQTT
byte willQoS = 0;
const char* willMessage = "Offline";
boolean willRetain = true;
std::string mqtt_tag;
int Mqtt_sendInterval = 120000;   // in milliseconds = 2 minutes
long Mqtt_lastScan = 0;
long lastReconnectAttempt = 0;
int Mqtt_reconnect = 0;

// Define NTP Client to get time
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;
int UTC_syncIntervall = 3600000; // in milliseconds = 1 hour
long UTC_lastSync;
time_t UTC_time;
long My_time = 0;

// Initializes the espClient. 
WiFiClient myClient;
PubSubClient client(myClient);
// name used as Mqtt tag
std::string gateway = HOSTNAME ;  

// Timers auxiliar variables
long now = millis();
char strtime[8];
long LEDblink = 0;
bool led = LOW;
int gpioLed = 1;
int LedBlinkTime = 1000;
int RelayResetTime = 5000;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);

// Create a WebSocket object
AsyncWebSocket ws("/ws");

// Set number of outputs
#define NUM_OUTPUTS  7      // nur 7 ansprechbare Relais !!!

// Assign each GPIO to an output
int outputGPIOs[NUM_OUTPUTS] =  {16, 17, 18, 19, 21, 22, 23};

// Assign relay details
String relayReset[NUM_OUTPUTS] = {"N", "N", "N", "N", "N", "N", "N"};
int relayResetStatus[NUM_OUTPUTS] = {0,0,0,0,0,0,0};
int relayResetTimer[NUM_OUTPUTS] = {0,0,0,0,0,0,0};

// end of definitions -----------------------------------------------------


// Initialize SPIFFS
void initSPIFFS() {
  if (!SPIFFS.begin()) {
    log_i("An error has occurred while mounting LittleFS");
  }
  log_i("LittleFS mounted successfully");
}

// init NTP-server
void initNTP() {
  log_i("init NTP-server");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // update ESP-systemtime to UTC
  delay(100);                                                 // udate takes some time
  time(&UTC_time);
  itoa(UTC_time,strtime,10);
  log_i("%s","Unix-timestamp =" );
  log_i("%s",strtime);
}

// Initialize WiFi
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(ssid, password);
  log_i("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    log_i(".");
    delay(1000);
  }
  log_i("%s",WiFi.localIP().toString().c_str());
}

String getOutputStates(){
  JSONVar myArray;

  myArray["cards"][0]["c_text"] = String(HOSTNAME) + "   /   " + String(VERSION);
  myArray["cards"][1]["c_text"] = willTopic;
  myArray["cards"][2]["c_text"] = String(WiFi.RSSI());
  myArray["cards"][3]["c_text"] = String(Mqtt_sendInterval) + "ms";
  myArray["cards"][4]["c_text"] = String(My_time);
  myArray["cards"][5]["c_text"] = "WiFi = " + String(WiFi_reconnect) + "   MQTT = " + String(Mqtt_reconnect);
  myArray["cards"][6]["c_text"] = " ";
  myArray["cards"][7]["c_text"] = " to reboot click ok";

  for (int i =0; i<NUM_OUTPUTS; i++){
    myArray["gpios"][i]["output"] = String(i);

    if (relayReset[i] == "Y") {
      myArray["gpios"][i]["state"] = String(digitalRead(outputGPIOs[i]));
    }
      else {
      myArray["gpios"][i]["state"] = String(!digitalRead(outputGPIOs[i]));
    }
  }
  String jsonString = JSON.stringify(myArray);
  return jsonString;
}

void notifyClients(String state) {
  ws.textAll(state);
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    
    data[len] = 0;

    //char help[30];
    //for (int i = 0; i <= len; i++){
    //  help[i] = data[i];
    //}
    //log_i("Data received: ");
    //log_i("%s\n",help);

    if (strcmp((char*)data, "states") == 0) {
      notifyClients(getOutputStates());
    }
    else{
      if (strcmp((char*)data, "Reboot") == 0) {
        log_i("Reset..");
        ESP.restart();
      }
      else {
        int gpio = atoi((char*)data);
        digitalWrite(outputGPIOs[gpio], !digitalRead(outputGPIOs[gpio]));
        notifyClients(getOutputStates());
        log_i("switch Relais");

        if (relayReset[gpio] == "Y") {
          relayResetStatus[gpio] = 1;
        }
      }
    }
  }

  Mqtt_lastScan = now - Mqtt_sendInterval - 10;  // --> MQTT send !!
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  switch (type) {
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

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

// reconnect to WiFi 
void reconnect_wifi() {
  log_i("%s\n","WiFi try reconnect"); 
  WiFi.begin();
  delay(500);
  if (WiFi.status() == WL_CONNECTED) {
    WiFi_reconnect = WiFi_reconnect + 1;
    // Once connected, publish an announcement...
    log_i("%s\n","WiFi reconnected"); 
  }
}

// This functions reconnects your ESP32 to your MQTT broker

void reconnect_mqtt() {
  if (client.connect(gateway.c_str(), willTopic, willQoS, willRetain, willMessage)) {
    // Once connected, publish an announcement...
    log_i("%s\n","Mqtt connected"); 
    mqtt_tag = gateway + "/connect";
    client.publish(mqtt_tag.c_str(),"connected");
    log_i("%s",mqtt_tag.c_str());
    log_i("%s\n","connected");
    mqtt_tag = "tele/" + gateway  + "/LWT";
    client.publish(mqtt_tag.c_str(),"Online",willRetain);
    log_i("%s",mqtt_tag.c_str());
    log_i("%s\n","Online");

    mqtt_tag = "cmnd/" + gateway + "/#";
    client.subscribe(mqtt_tag.c_str());

    Mqtt_reconnect = Mqtt_reconnect + 1;
  }
}

// receive MQTT messages
void MQTT_callback(char* topic, byte* message, unsigned int length) {
  
  log_i("%s","Message arrived on topic: ");
  log_i("%s\n",topic);
  log_i("%s","Data : ");

  String MQTT_message;
  for (int i = 0; i < length; i++) {
    MQTT_message += (char)message[i];
  }
  log_i("%s\n",MQTT_message);

  String Topic_Relais1 = String("cmnd/"); 
  Topic_Relais1 = String(Topic_Relais1 + gateway.c_str() + "/Relais1");
  String Topic_Relais2 = String("cmnd/"); 
  Topic_Relais2 = String(Topic_Relais2 + gateway.c_str() + "/Relais2");
  String Topic_Relais3 = String("cmnd/"); 
  Topic_Relais3 = String(Topic_Relais3 + gateway.c_str() + "/Relais3");
  String Topic_Relais4 = String("cmnd/"); 
  Topic_Relais4 = String(Topic_Relais4 + gateway.c_str() + "/Relais4");
  String Topic_Relais5 = String("cmnd/"); 
  Topic_Relais5 = String(Topic_Relais5 + gateway.c_str() + "/Relais5");
  String Topic_Relais6 = String("cmnd/"); 
  Topic_Relais6 = String(Topic_Relais6 + gateway.c_str() + "/Relais6");
  String Topic_Relais7 = String("cmnd/"); 
  Topic_Relais7 = String(Topic_Relais7 + gateway.c_str() + "/Relais7");

  if (String(topic) == Topic_Relais1 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[0], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[0], HIGH);
    }
  }
    if (String(topic) == Topic_Relais2 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[1], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[1], HIGH);
    }
  }
    if (String(topic) == Topic_Relais3 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[2], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[2], HIGH);
    }
  }
  if (String(topic) == Topic_Relais4 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[3], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[3], HIGH);
    }
  }
    if (String(topic) == Topic_Relais5 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[4], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[4], HIGH);
    }
  }
    if (String(topic) == Topic_Relais6 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[5], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[5], HIGH);
    }
  }
    if (String(topic) == Topic_Relais7 ){
    if(MQTT_message == "on"){
      digitalWrite(outputGPIOs[6], LOW);
    }
    else if(MQTT_message == "off"){
      digitalWrite(outputGPIOs[6], HIGH);
    }
  }

  notifyClients(getOutputStates());

}

void MQTTsend () {
  JSONVar mqtt_data; 
  
  mqtt_tag = "tele/" + gateway + "/SENSOR";
  log_i("%s\n",mqtt_tag.c_str());

  mqtt_data["Time"] = My_time;
  mqtt_data["RSSI"] = WiFi.RSSI();

  mqtt_data["Relais1"] = digitalRead(outputGPIOs[0]);
  mqtt_data["Relais2"] = digitalRead(outputGPIOs[1]);
  mqtt_data["Relais3"] = digitalRead(outputGPIOs[2]);
  mqtt_data["Relais4"] = digitalRead(outputGPIOs[3]);
  mqtt_data["Relais5"] = digitalRead(outputGPIOs[4]);
  mqtt_data["Relais6"] = digitalRead(outputGPIOs[5]);
  mqtt_data["Relais7"] = digitalRead(outputGPIOs[6]);

  String mqtt_string = JSON.stringify(mqtt_data);

  log_i("%s\n",mqtt_string.c_str()); 

  client.publish(mqtt_tag.c_str(), mqtt_string.c_str());

  notifyClients(getOutputStates());
}

void setup(){

  SERIALINIT

  log_i("init GPIOs\n");
  pinMode(gpioLed, OUTPUT);
  digitalWrite(gpioLed,LOW);

  // Set GPIOs as outputs. Set all relays to off when the program starts -  the relay is off when you set the relay to HIGH
  for (int i =0; i<NUM_OUTPUTS; i++){
    pinMode(outputGPIOs[i], OUTPUT);
    digitalWrite(outputGPIOs[i], HIGH);
  }

  initSPIFFS();
  initWiFi();
  initNTP();
  initWebSocket();

  log_i("setup MQTT\n");
  client.setServer(mqtt_server, 1883);
  client.setCallback(MQTT_callback);


  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html",false);
  });

  server.serveStatic("/", SPIFFS, "/");

  // Start ElegantOTA
  AsyncElegantOTA.begin(&server);
  
  // Start server
  server.begin();
}

void loop() {
  
  ws.cleanupClients();

  now = millis();

  // LED blinken und update My_time mit UTC

    time(&UTC_time);                      // nicht zu oft aufrufen!! bremst den Web-Server massiv aus ???
    My_time = UTC_time;

    if (now - LEDblink > LedBlinkTime) {
      LEDblink = now;
      if(led == 0) {
       digitalWrite(gpioLed, HIGH);
       led = 1;
      }else{
       digitalWrite(gpioLed, LOW);
       led = 0;
      }
    }

  // auf Reset prüfen
  // falls nötig Timer setzten
    for(int i=0; i<NUM_OUTPUTS; i++){
      if (relayResetStatus[i] == 1) {
        relayResetStatus[i] = 2;
        relayResetTimer[i] = now;
      }
    }

  // prüfen ob Timer abgelaufen; wenn ja Relais ausschalten
    for(int i=0; i<NUM_OUTPUTS; i++){
      if (relayResetStatus[i] == 2 ){
        if (now - relayResetTimer[i] > RelayResetTime ){
          relayResetStatus[i] = 0;
          digitalWrite(outputGPIOs[i], HIGH);
          notifyClients(getOutputStates());
          Mqtt_lastScan = now - Mqtt_sendInterval - 10;  // --> MQTT send !!
        }
      }
    }  

  // perform UTC sync
  if (now - UTC_lastSync > UTC_syncIntervall) {
    UTC_lastSync = now;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // update ESP-systemtime to UTC
    delay(100);                                                 // udate takes some time
    time(&UTC_time);
    log_i("%s","Re-sync ESP-time!! Unix-timestamp =");
    itoa(UTC_time,strtime,10);
    log_i("%s",strtime);
  }   

    // check WiFi
    if (WiFi.status() != WL_CONNECTED  ) {
      // try reconnect every 5 seconds
      if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;              // prevents mqtt reconnect running also
        // Attempt to reconnect
        log_i("WiFi reconnect"); 
        reconnect_wifi();
      }
    }

  // check if MQTT broker is still connected
    if (!client.connected()) {
      // try reconnect every 5 seconds
      if (now - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = now;
        // Attempt to reconnect
        log_i("MQTT reconnect"); 
        reconnect_mqtt();
      }
    } else {
      // Client connected

      client.loop();

      // send data to MQTT broker
      if (now - Mqtt_lastScan > Mqtt_sendInterval) {
      Mqtt_lastScan = now;
      MQTTsend();
      } 
    }
}
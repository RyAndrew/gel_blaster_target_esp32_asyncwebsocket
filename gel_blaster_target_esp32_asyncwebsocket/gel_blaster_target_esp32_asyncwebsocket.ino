#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFSEditor.h>
#include <Servo.h>

#include <AsyncJson.h>
#include <ArduinoJson.h>

const char* ssid = "wifinamehere";
const char* password = "wifipasswordhere";
const char * hostName = "esp-async";
const char* http_username = "admin";
const char* http_password = "admin";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

const int LedStatusPin = 2;
int LedStatusState = HIGH;

int tempCounter = 0;
int tempCounterReset = 4;
int espTemp = 4;

const uint16_t targetUpServoMs = 1800;
const uint16_t targetDownServoMs = 800;

const uint16_t lightSensorTriggerMax = 300;

const uint8_t lightSensor1Pin = 36;
const uint8_t lightSensor2Pin = 39;
const uint8_t lightSensor3Pin = 34;
const uint8_t lightSensor4Pin = 35;
const uint8_t lightSensor5Pin = 32;
const uint8_t lightSensor6Pin = 33;

static const uint8_t servo1Pin = 25;
static const uint8_t servo2Pin = 26;
static const uint8_t servo3Pin = 27;
static const uint8_t servo4Pin = 14;
static const uint8_t servo5Pin = 12;
static const uint8_t servo6Pin = 13;

struct target {
  uint8_t number;
  uint16_t lightSensorValue;
  uint8_t lightSensorPin;
  uint8_t servoPin;
  uint8_t servoResetCounter;
  Servo servo;
};

static const uint8_t targetCount = 6;

target allTargets[targetCount] = {
  { 1, 0, lightSensor1Pin, servo1Pin, 0 },
  { 2, 0, lightSensor2Pin, servo2Pin, 0 },
  { 3, 0, lightSensor3Pin, servo3Pin, 0 },
  { 4, 0, lightSensor4Pin, servo4Pin, 0 },
  { 5, 0, lightSensor5Pin, servo5Pin, 0 },
  { 6, 0, lightSensor6Pin, servo6Pin, 0 }
};

volatile int interruptCounter;
 
hw_timer_t * timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
 

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  interruptCounter++;
  portEXIT_CRITICAL_ISR(&timerMux);
 
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    client->printf("Hello Client %u :)", client->id());
    client->ping();
  } else if(type == WS_EVT_DISCONNECT){
    Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
  } else if(type == WS_EVT_ERROR){
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  } else if(type == WS_EVT_PONG){
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len)?(char*)data:"");
  } else if(type == WS_EVT_DATA){
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if(info->final && info->index == 0 && info->len == len){
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);

      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < info->len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for(size_t i=0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n",msg.c_str());

      if(info->opcode == WS_TEXT)
        client->text("I got your text message");
      else
        client->binary("I got your binary message");
    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if(info->index == 0){
        if(info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT)?"text":"binary", info->index, info->index + len);

      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for(size_t i=0; i < len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n",msg.c_str());

      if((info->index + len) == info->len){
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if(info->final){
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
          if(info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}


void setup(){
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  Serial.println("Booting");

  for (int i = 0; i < targetCount; i++){
    allTargets[i].servo.attach(allTargets[i].servoPin,Servo::CHANNEL_NOT_ATTACHED, 0, 180, 0, 2400);
    allTargets[i].servo.writeMicroseconds(800);
  }
//  delay(100);
//   for (int i = 0; i < targetCount; i++){
//    allTargets[i].servo.writeMicroseconds(0);
//  }
  
  //analogReadResolution(11);
  //analogSetAttenuation(ADC_6db);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  //WiFi.softAP(hostName);
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.printf("STA: Failed!\n");
    WiFi.disconnect(false);
    delay(1000);
    WiFi.begin(ssid, password);
  }

  //Send OTA events to the browser
  ArduinoOTA.onStart([]() { events.send("Update Start", "ota"); });
  ArduinoOTA.onEnd([]() { events.send("Update End", "ota"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char p[32];
    sprintf(p, "Progress: %u%%\n", (progress/(total/100)));
    events.send(p, "ota");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    if(error == OTA_AUTH_ERROR) events.send("Auth Failed", "ota");
    else if(error == OTA_BEGIN_ERROR) events.send("Begin Failed", "ota");
    else if(error == OTA_CONNECT_ERROR) events.send("Connect Failed", "ota");
    else if(error == OTA_RECEIVE_ERROR) events.send("Recieve Failed", "ota");
    else if(error == OTA_END_ERROR) events.send("End Failed", "ota");
  });
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.begin();

  MDNS.addService("http","tcp",80);

  SPIFFS.begin();

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  events.onConnect([](AsyncEventSourceClient *client){
    client->send("hello!",NULL,millis(),1000);
  });
  server.addHandler(&events);

  server.addHandler(new SPIFFSEditor(SPIFFS, http_username,http_password));

  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");

  server.onNotFound([](AsyncWebServerRequest *request){
    Serial.printf("NOT_FOUND: ");
    if(request->method() == HTTP_GET)
      Serial.printf("GET");
    else if(request->method() == HTTP_POST)
      Serial.printf("POST");
    else if(request->method() == HTTP_DELETE)
      Serial.printf("DELETE");
    else if(request->method() == HTTP_PUT)
      Serial.printf("PUT");
    else if(request->method() == HTTP_PATCH)
      Serial.printf("PATCH");
    else if(request->method() == HTTP_HEAD)
      Serial.printf("HEAD");
    else if(request->method() == HTTP_OPTIONS)
      Serial.printf("OPTIONS");
    else
      Serial.printf("UNKNOWN");
    Serial.printf(" http://%s%s\n", request->host().c_str(), request->url().c_str());

    if(request->contentLength()){
      Serial.printf("_CONTENT_TYPE: %s\n", request->contentType().c_str());
      Serial.printf("_CONTENT_LENGTH: %u\n", request->contentLength());
    }

    int headers = request->headers();
    int i;
    for(i=0;i<headers;i++){
      AsyncWebHeader* h = request->getHeader(i);
      Serial.printf("_HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
    }

    int params = request->params();
    for(i=0;i<params;i++){
      AsyncWebParameter* p = request->getParam(i);
      if(p->isFile()){
        Serial.printf("_FILE[%s]: %s, size: %u\n", p->name().c_str(), p->value().c_str(), p->size());
      } else if(p->isPost()){
        Serial.printf("_POST[%s]: %s\n", p->name().c_str(), p->value().c_str());
      } else {
        Serial.printf("_GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
      }
    }

    request->send(404);
  });
  server.onFileUpload([](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final){
    if(!index)
      Serial.printf("UploadStart: %s\n", filename.c_str());
    Serial.printf("%s", (const char*)data);
    if(final)
      Serial.printf("UploadEnd: %s (%u)\n", filename.c_str(), index+len);
  });
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    if(!index)
      Serial.printf("BodyStart: %u\n", total);
    Serial.printf("%s", (const char*)data);
    if(index + len == total)
      Serial.printf("BodyEnd: %u\n", total);
  });
  server.begin();

  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 250000, true);
  timerAlarmEnable(timer);
  
  digitalWrite(LedStatusPin,  LedStatusState);

  pinMode(LedStatusPin,OUTPUT);
}


void run250msloop(){

  if(tempCounter==0){
    espTemp = temperatureRead();
    tempCounter = tempCounterReset;
  }
  tempCounter--;
    
    LedStatusState = not(LedStatusState);
    digitalWrite(LedStatusPin,  LedStatusState);

    for (int i = 0; i < targetCount; i++){
//        Serial.print("Read Pin ");
//        Serial.print(allTargets[i].lightSensorPin);
//        Serial.print(" = ");
//        Serial.println(allTargets[i].lightSensorValue);
        
      allTargets[i].lightSensorValue = analogRead(allTargets[i].lightSensorPin);
      if(allTargets[i].lightSensorValue < lightSensorTriggerMax){
        allTargets[i].servoResetCounter = 4;
        
//        Serial.print("Servo ");
//        Serial.print(i+1);
//        Serial.println(" Trigger");
      }
      if(allTargets[i].servoResetCounter>0){
        
        allTargets[i].servoResetCounter--;
        if(allTargets[i].servoResetCounter==3){
          
//          Serial.print("Servo ");
//          Serial.print(i+1);
//          Serial.println(" Up");
        
          allTargets[i].servo.writeMicroseconds(targetUpServoMs);
        }
        if(allTargets[i].servoResetCounter==1){
//          Serial.print("Servo ");
//          Serial.print(i+1);
//          Serial.println(" Down");
          allTargets[i].servo.writeMicroseconds(targetDownServoMs);
        }
      }
    }
    
    const int capacity = JSON_OBJECT_SIZE(20);
    StaticJsonDocument<capacity> json;
    json["espTemp"] = espTemp;

    char sensorName[13];
    for (int i = 0; i < targetCount; i++){
      sprintf(sensorName, "lightSensor%d", i + 1);
      json[sensorName] = allTargets[i].lightSensorValue;
    }
    
    char jsonOutput[128];
    serializeJson(json, jsonOutput);
    ws.textAll(jsonOutput);

    Serial.println(jsonOutput);

}

void loop(){
  ArduinoOTA.handle();

  if (interruptCounter > 0) {
 
    portENTER_CRITICAL(&timerMux);
    interruptCounter = 0;
    portEXIT_CRITICAL(&timerMux);

    run250msloop();
 
  }

}

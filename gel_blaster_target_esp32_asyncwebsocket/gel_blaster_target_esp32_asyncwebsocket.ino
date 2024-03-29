#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
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

const uint8_t MODE_AUTO = 1;
const uint8_t MODE_MANUAL = 2;
const uint8_t MODE_ALL = 3;
uint8_t currentMode = MODE_AUTO;

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
  uint8_t down;
  uint8_t enabled;
  uint16_t lightSensorValue;
  uint8_t lightSensorPin;
  uint8_t servoPin;
  uint8_t servoResetCounter;
  Servo servo;
};

static const uint8_t targetCount = 6;

target allTargets[targetCount] = {
  { 1, 0, 0, 0, lightSensor1Pin, servo1Pin, 0 },
  { 2, 0, 0, 0, lightSensor2Pin, servo2Pin, 0 },
  { 3, 0, 0, 0, lightSensor3Pin, servo3Pin, 0 },
  { 4, 0, 0, 0, lightSensor4Pin, servo4Pin, 0 },
  { 5, 0, 0, 0, lightSensor5Pin, servo5Pin, 0 },
  { 6, 0, 0, 0, lightSensor6Pin, servo6Pin, 0 }
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

      if(info->opcode == WS_TEXT){
        client->text("{\"success\":true}");
        Serial.println("recvd websocket message!");
        Serial.println(msg);
        handleClientJsonData(msg);
      }else{
            client->binary("I got your binary message");
      }
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
          if(info->message_opcode == WS_TEXT){
              client->text("{\"success\":true}");
              Serial.println("recvd websocket message!");
              Serial.println(msg);
              handleClientJsonData(msg);
          }else{
            client->binary("I got your binary message");
          }
        }
      }
    }
  }
}

void handleClientJsonData(String data){
  StaticJsonDocument<200> decodedJson;
  
  DeserializationError errorDecode = deserializeJson(decodedJson, data);
  // Test if parsing succeeds.
  if (errorDecode) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(errorDecode.c_str());
    return;
  }
  //JsonObject decodedJsonObject = decodedJsonString.to<JsonObject>();
    JsonVariant jsonCmd = decodedJson["cmd"];
    if (jsonCmd.isNull()) {
      Serial.println("cmd not found");
      return;
    }
    Serial.println("json cmd received!");
    String cmdString = jsonCmd.as<String>();

    Serial.println(cmdString);
    
    if(cmdString == "mode"){
      JsonVariant jsonValue = decodedJson["value"];
      if (jsonValue.isNull()){
        Serial.println("mode value not found");
        return;
      }
      
      if(jsonValue.as<String>() == "all"){
        Serial.println("command mode = all");
        currentMode = MODE_ALL;
        return;
      }
      if(jsonValue.as<String>() == "auto"){
        Serial.println("command mode = auto");
        currentMode = MODE_AUTO;
        return;
      }
      if(jsonValue.as<String>() == "manual"){
        Serial.println("command mode = manual");
        currentMode = MODE_MANUAL;
        return;
      }
      Serial.println("mode invalid value");
      return;
    }
    if(cmdString == "targetUp"){
      JsonVariant jsonValue = decodedJson["value"];
      if (jsonValue.isNull()){
        Serial.println("targetUp value not found");
        return;
      }
      if(jsonValue.as<String>() == "all"){
        Serial.println("command targetUp = all");
        putAllTargetsUp();
        return;
      }else{
       int targetNo = jsonValue.as<unsigned int>();
       if(targetNo == 0 || targetNo > 6){
        Serial.print("command targetUp value invalid");
        return;
        }
        Serial.print("command targetUp = ");
        Serial.println(targetNo);
        allTargets[targetNo-1].servoResetCounter = 4;
      }
    }
}

void setup(){
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  Serial.println(F("Booting"));

  for (uint8_t i = 0; i < targetCount; i++){
    allTargets[i].servo.attach(allTargets[i].servoPin,Servo::CHANNEL_NOT_ATTACHED, 0, 180, 0, 2400);
    allTargets[i].servo.writeMicroseconds(800);
  }
//  delay(100);
//   for (uint8_t i = 0; i < targetCount; i++){
//    allTargets[i].servo.writeMicroseconds(0);
//  }
  
  //analogReadResolution(11);
  //analogSetAttenuation(ADC_6db);

  char mac[18];
  WiFi.macAddress().toCharArray(mac,18);
  
  Serial.print(F("Wifi Mac: ")); Serial.println(mac);
  
  char hostName[24];
  sprintf(hostName,"MagicTarget%c%c%c%c",mac[12],mac[13],mac[15],mac[16]);
  Serial.print(F("HostName: ")); Serial.println(hostName);

WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  //WiFi.softAP(hostName);
  
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(hostName);

  WiFi.begin(ssid, password);

  
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println(F("Wifi STA: Failed!\n"));
    WiFi.disconnect(false);
    delay(1000);
    WiFi.begin(ssid, password);
  }else{
    Serial.println(F("Wifi Connected!"));
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }

  //Send OTA events to the browser
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";
        SPIFFS.end();

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.setPassword("fart");
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

  String configFile = "config.json";
  
  if (SPIFFS.exists(configFile)) {
    File file = SPIFFS.open(configFile, "r");
    uint16_t configFileSize = file.size();
    char configFileContents[configFileSize];
    file.readBytes(configFileContents, configFileSize);
     file.close();
     Serial.println(configFileContents);
  }else{
    Serial.print("config file ");
    Serial.print(configFile);
    Serial.println(" not found");
  }


    for (uint8_t i = 0; i < targetCount; i++){
//        Serial.print("Read Pin ");
//        Serial.print(allTargets[i].lightSensorPin);
//        Serial.print(" = ");
//        Serial.println(allTargets[i].lightSensorValue);
        
      allTargets[i].lightSensorValue = analogRead(allTargets[i].lightSensorPin);

      Serial.print("Target ");
      Serial.print(i+1);
      if(allTargets[i].lightSensorValue > 400){
        allTargets[i].enabled = 1;
        Serial.println(" Enabled");
      }else{
        allTargets[i].lightSensorValue = 0;
        Serial.println(" Disabled");
      }
    }
  
}

void sendTargetJammedMessage(int targetNo){

    const int capacity = JSON_OBJECT_SIZE(2);
    StaticJsonDocument<capacity> json;
    json["cmd"] = "targetJammed";
    json["target"] = targetNo;
    
    char jsonOutput[40];
    serializeJson(json, jsonOutput);
    ws.textAll(jsonOutput);

    Serial.println(jsonOutput);

}
void sendTargetDownMessage(int targetNo){

    const int capacity = JSON_OBJECT_SIZE(2);
    StaticJsonDocument<capacity> json;
    json["cmd"] = "targetDown";
    json["target"] = targetNo;
    
    char jsonOutput[40];
    serializeJson(json, jsonOutput);
    ws.textAll(jsonOutput);

    Serial.println(jsonOutput);

}
void putAllTargetsUp(){
    for (uint8_t i = 0; i < targetCount; i++){
      if(allTargets[i].enabled == 1){
        allTargets[i].servoResetCounter = 4;
      }
    }
}
void checkIfAllKnocked(){
    uint8_t knockedCount = 0;
    uint8_t enabledCount = 0;
    for (uint8_t i = 0; i < targetCount; i++){
      if(allTargets[i].enabled == 1){
        enabledCount++;
      }else{
        continue;
      }
      if(allTargets[i].down == 1){
        knockedCount++;
      }
    }
    if(knockedCount == enabledCount){
      putAllTargetsUp();
    }
}
void run250msloop(){

  if(tempCounter==0){
    espTemp = temperatureRead();
    tempCounter = tempCounterReset;
  }
  tempCounter--;
    
    LedStatusState = not(LedStatusState);
    digitalWrite(LedStatusPin,  LedStatusState);

    for (uint8_t i = 0; i < targetCount; i++){
//        Serial.print("Read Pin ");
//        Serial.print(allTargets[i].lightSensorPin);
//        Serial.print(" = ");
//        Serial.println(allTargets[i].lightSensorValue);

      if(allTargets[i].enabled == 0){
        continue;
      }

        allTargets[i].lightSensorValue = analogRead(allTargets[i].lightSensorPin);
        
        if( allTargets[i].lightSensorValue >= lightSensorTriggerMax){
          allTargets[i].down = 0;
        }else{
          if(allTargets[i].down == 0){
            sendTargetDownMessage(i+1);
  
            allTargets[i].down = 1;
            //only check if all knocked if we have a status change
            if(currentMode == MODE_ALL){
              checkIfAllKnocked();
            }
            
            if(currentMode == MODE_AUTO){
              allTargets[i].servoResetCounter = 4;
            }
          }
            
          allTargets[i].down = 1;
    
    
    //        Serial.print("Target ");
    //        Serial.print(i+1);
    //        Serial.println(" Trigger");
        }

      if(allTargets[i].servoResetCounter>0){
        
        allTargets[i].servoResetCounter--;
        if(allTargets[i].servoResetCounter==3){
          
//          Serial.print("Target ");
//          Serial.print(i+1);
//          Serial.println(" Up");
        
          allTargets[i].servo.writeMicroseconds(targetUpServoMs);
        }
        if(allTargets[i].servoResetCounter==1){
//          Serial.print("Target ");
//          Serial.print(i+1);
//          Serial.println(" Down");
          allTargets[i].servo.writeMicroseconds(targetDownServoMs);
        }
      }else{
        if(currentMode == MODE_AUTO && allTargets[i].down == 1){
          
          Serial.print("Target ");
          Serial.print(i+1);
          Serial.println(" May Be Jammed");
          sendTargetJammedMessage(i+1);
        }
      }
    }
    
    const int capacity = JSON_OBJECT_SIZE(20);
    StaticJsonDocument<capacity> json;
    json["espTemp"] = espTemp;

    char sensorName[13];
    for (uint8_t i = 0; i < targetCount; i++){
      sprintf(sensorName, "lightSensor%d", i + 1);
      json[sensorName] = allTargets[i].lightSensorValue;
    }
    
    char jsonOutput[128];
    serializeJson(json, jsonOutput);
//    ws.textAll(jsonOutput);

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

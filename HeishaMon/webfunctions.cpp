#include "webfunctions.h"
#include "decode.h"
#include "version.h"
#include "commands.h"
#include "src/common/progmem.h"
#include "src/common/timerqueue.h"

#include "lwip/apps/sntp.h"
#include "lwip/dns.h"

#include <WiFi.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <time.h>
#include <Update.h>

#define UPTIME_OVERFLOW 4294967295 // Uptime overflow value

static String wifiJsonList = "";

static uint8_t ntpservers = 0;

void log_message(char* string);
void log_message(const __FlashStringHelper *msg);

int dBmToQuality(int dBm) {
  if (dBm == 31)
    return -1;
  if (dBm <= -100)
    return 0;
  if (dBm >= -50)
    return 100;
  return 2 * (dBm + 100);
}

int getWifiQuality() {
  if (WiFi.status() != WL_CONNECTED)
    return -1;
  return dBmToQuality(WiFi.RSSI());
}

int getFreeMemory() {
  //store total memory at boot time
  static uint32_t total_memory = 0;
  if ( 0 == total_memory ) total_memory = ESP.getFreeHeap();

  uint32_t free_memory   = ESP.getFreeHeap();
  return (100 * free_memory / total_memory ) ; // as a %
}


void loadSettings(settingsStruct *heishamonSettings) {
  //read configuration from FS json
  log_message(_F("mounting FS..."));

  if (LittleFS.begin()) {
    log_message(_F("mounted file system"));
    if (LittleFS.exists("/config.json")) {
      //file exists, reading and loading
      log_message(_F("reading config file"));
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        log_message(_F("opened config file"));
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument jsonDoc(1024);
        DeserializationError error = deserializeJson(jsonDoc, buf.get());
        char log_msg[1024];
        serializeJson(jsonDoc, log_msg);
        log_message(log_msg);
        if (!error) {
          log_message(_F("parsed json"));
          //read updated parameters, make sure no overflow
          if ( jsonDoc["wifi_ssid"] ) strncpy(heishamonSettings->wifi_ssid, jsonDoc["wifi_ssid"], sizeof(heishamonSettings->wifi_ssid));
          if ( jsonDoc["wifi_password"] ) strncpy(heishamonSettings->wifi_password, jsonDoc["wifi_password"], sizeof(heishamonSettings->wifi_password));
          if ( jsonDoc["wifi_hostname"] ) strncpy(heishamonSettings->wifi_hostname, jsonDoc["wifi_hostname"], sizeof(heishamonSettings->wifi_hostname));
          if ( jsonDoc["ota_password"] ) strncpy(heishamonSettings->ota_password, jsonDoc["ota_password"], sizeof(heishamonSettings->ota_password));
          if ( jsonDoc["mqtt_topic_base"] ) strncpy(heishamonSettings->mqtt_topic_base, jsonDoc["mqtt_topic_base"], sizeof(heishamonSettings->mqtt_topic_base));
          if ( jsonDoc["mqtt_topic_listen"] ) strncpy(heishamonSettings->mqtt_topic_listen, jsonDoc["mqtt_topic_listen"], sizeof(heishamonSettings->mqtt_topic_listen));
          if ( jsonDoc["mqtt_server"] ) strncpy(heishamonSettings->mqtt_server, jsonDoc["mqtt_server"], sizeof(heishamonSettings->mqtt_server));
          if ( jsonDoc["mqtt_port"] ) strncpy(heishamonSettings->mqtt_port, jsonDoc["mqtt_port"], sizeof(heishamonSettings->mqtt_port));
          if ( jsonDoc["mqtt_username"] ) strncpy(heishamonSettings->mqtt_username, jsonDoc["mqtt_username"], sizeof(heishamonSettings->mqtt_username));
          if ( jsonDoc["mqtt_password"] ) strncpy(heishamonSettings->mqtt_password, jsonDoc["mqtt_password"], sizeof(heishamonSettings->mqtt_password));
          if ( jsonDoc["ntp_servers"] ) strncpy(heishamonSettings->ntp_servers, jsonDoc["ntp_servers"], sizeof(heishamonSettings->ntp_servers));
          if ( jsonDoc["timezone"]) heishamonSettings->timezone = jsonDoc["timezone"];
          heishamonSettings->listenonly = ( jsonDoc["listenonly"] == "enabled" ) ? true : false;
          heishamonSettings->listenmqtt = ( jsonDoc["listenmqtt"] == "enabled" ) ? true : false;
          heishamonSettings->logMqtt = ( jsonDoc["logMqtt"] == "enabled" ) ? true : false;
          heishamonSettings->logHexdump = ( jsonDoc["logHexdump"] == "enabled" ) ? true : false;
          heishamonSettings->logSerial1 = ( jsonDoc["logSerial1"] == "enabled" ) ? true : false;
          heishamonSettings->optionalPCB = ( jsonDoc["optionalPCB"] == "enabled" ) ? true : false;
          if ( jsonDoc["waitTime"]) heishamonSettings->waitTime = jsonDoc["waitTime"];
          if (heishamonSettings->waitTime < 5) heishamonSettings->waitTime = 5;
          if ( jsonDoc["waitDallasTime"]) heishamonSettings->waitDallasTime = jsonDoc["waitDallasTime"];
          if (heishamonSettings->waitDallasTime < 5) heishamonSettings->waitDallasTime = 5;
          if ( jsonDoc["dallasResolution"]) heishamonSettings->dallasResolution = jsonDoc["dallasResolution"];
          if ((heishamonSettings->dallasResolution < 9) || (heishamonSettings->dallasResolution > 12) ) heishamonSettings->dallasResolution = 12;
          if ( jsonDoc["updateAllTime"]) heishamonSettings->updateAllTime = jsonDoc["updateAllTime"];
          if (heishamonSettings->updateAllTime < heishamonSettings->waitTime) heishamonSettings->updateAllTime = heishamonSettings->waitTime;
          if ( jsonDoc["updataAllDallasTime"]) heishamonSettings->updataAllDallasTime = jsonDoc["updataAllDallasTime"];
          if (heishamonSettings->updataAllDallasTime < heishamonSettings->waitDallasTime) heishamonSettings->updataAllDallasTime = heishamonSettings->waitDallasTime;
        } else {
          log_message(_F("Failed to load json config, forcing config reset."));
          WiFi.persistent(true);
          WiFi.disconnect();
          WiFi.persistent(false);
        }
        configFile.close();
      }
    }
    else {
      log_message(_F("No config.json exists! Forcing a config reset."));
      WiFi.persistent(true);
      WiFi.disconnect();
      WiFi.persistent(false);
    }
  } else {
    log_message(_F("failed to mount FS"));
  }
  //end read

}

void setupWifi(settingsStruct *heishamonSettings) {
  log_message(_F("Wifi reconnecting with new configuration..."));
  //no sleep wifi
  // ESP32:Disabled
  // WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);

  if (heishamonSettings->wifi_ssid[0] != '\0') {
    log_message(_F("Wifi client mode..."));
    //WiFi.persistent(true); //breaks stuff

    if (heishamonSettings->wifi_password[0] == '\0') {
      WiFi.begin(heishamonSettings->wifi_ssid);
    } else {
      WiFi.begin(heishamonSettings->wifi_ssid, heishamonSettings->wifi_password);
    }
  }
  else {
    log_message(_F("Wifi hotspot mode..."));
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(F("HeishaMon-Setup"));
  }

  if (heishamonSettings->wifi_hostname[0] == '\0') {
    //Set hostname on wifi rather than ESP_xxxxx
    WiFi.hostname(F("HeishaMon"));
  } else {
    WiFi.hostname(heishamonSettings->wifi_hostname);
  }
}


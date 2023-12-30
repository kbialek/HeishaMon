#define LWIP_INTERNAL

#include <Arduino.h>

#define HEATPUMP_VALUE_LEN    16

void log_message(char* string);

struct settingsStruct {
  uint16_t waitTime = 5; // how often data is read from heatpump
  uint16_t waitDallasTime = 5; // how often temps are read from 1wire
  uint16_t dallasResolution = 12; // dallas temp resolution (9 to 12)
  uint16_t updateAllTime = 300; // how often all data is resend to mqtt
  uint16_t updataAllDallasTime = 300; //how often all 1wire data is resent to mqtt
  uint16_t timezone = 0;

  const char* update_path = "/firmware";
  const char* update_username = "admin";
  char mqtt_server[64];
  char mqtt_port[6] = "1883";
  char mqtt_username[64];
  char mqtt_password[64];
  char mqtt_topic_base[128] = "panasonic_heat_pump";
  char mqtt_topic_listen[128] = "master_panasonic_heat_pump";
  char ntp_servers[254] = "pool.ntp.org";

  bool listenonly = true; //listen only so heishamon can be installed parallel to cz-taw1, set commands will not work though
  bool listenmqtt = false; //do we get heatpump data from another heishamon over mqtt?
  bool optionalPCB = false; //do we emulate an optional PCB?
  bool logMqtt = false; //log to mqtt from start
  bool logHexdump = false; //log hexdump from start
  bool logSerial1 = true; //log to serial1 (gpio2) from start
};

int getFreeMemory(void);
int getWifiQuality(void);
int getFreeMemory(void);

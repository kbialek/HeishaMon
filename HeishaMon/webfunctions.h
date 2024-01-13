#define LWIP_INTERNAL

#include <Arduino.h>

void log_message(char* string);

struct settingsStruct {
  uint16_t waitTime = 5; // how often data is read from heatpump
  uint16_t updateAllTime = 300; // how often all data is resend to mqtt

  char mqtt_port[6] = "1883";
  char mqtt_topic_base[128] = "home/hvac/heatpump/board";

  bool optionalPCB = true; //do we emulate an optional PCB?
  bool logMqtt = true; //log to mqtt from start
};

int getFreeMemory(void);
int getWifiQuality(void);
int getFreeMemory(void);

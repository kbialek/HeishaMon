#define LWIP_INTERNAL

#include <Arduino.h>

void log_message(char* string);

struct settingsStruct {
  uint16_t waitTime = 5; // how often data is read from heatpump
  uint16_t updateAllTime = 300; // how often all data is resend to mqtt

  char mqtt_port[6] = "1883";
  char mqtt_topic_base[128] = "panasonic_heat_pump";
  char mqtt_topic_listen[128] = "master_panasonic_heat_pump";

  bool listenonly = true; //listen only so heishamon can be installed parallel to cz-taw1, set commands will not work though
  bool optionalPCB = false; //do we emulate an optional PCB?
  bool logMqtt = false; //log to mqtt from start
  bool logHexdump = false; //log hexdump from start
};

int getFreeMemory(void);
int getWifiQuality(void);
int getFreeMemory(void);

#include "webfunctions.h"
// #include "decode.h"
// #include "version.h"
// #include "commands.h"
// #include "src/common/progmem.h"
// #include "src/common/timerqueue.h"

// #include "lwip/apps/sntp.h"
// #include "lwip/dns.h"

#include <WiFi.h>
// #include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
// #include <time.h>
// #include <Update.h>

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


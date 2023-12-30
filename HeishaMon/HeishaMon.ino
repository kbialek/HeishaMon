
#define LWIP_INTERNAL

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#include "src/common/timerqueue.h"
#include "src/common/stricmp.h"
#include "src/common/log.h"
#include "src/common/progmem.h"

#include "webfunctions.h"
#include "decode.h"
#include "commands.h"
#include "version.h"

#define SERIALTIMEOUT 2000 // wait until all 203 bytes are read, must not be too long to avoid blocking the code

settingsStruct heishamonSettings;

bool sending = false; // mutex for sending data
bool mqttcallbackinprogress = false; // mutex for processing mqtt callback

bool extraDataBlockAvailable = false; // this will be set to true if, during boot, heishamon detects this heatpump has extra data block (like K and L series do)
bool extraDataBlockChecked = false; // this will be true if we already checked for the extra data block

#define MQTTRECONNECTTIMER 30000 //it takes 30 secs for each mqtt server reconnect attempt
unsigned long lastMqttReconnectAttempt = 0;

#define WIFIRETRYTIMER 15000 // switch between hotspot and configured SSID each 10 secs if SSID is lost
unsigned long lastWifiRetryTimer = 0;

unsigned long lastRunTime = 0;
unsigned long lastOptionalPCBRunTime = 0;
unsigned long lastOptionalPCBSave = 0;

unsigned long sendCommandReadTime = 0; //set to millis value during send, allow to wait millis for answer
unsigned long goodreads = 0;
unsigned long totalreads = 0;
unsigned long badcrcread = 0;
unsigned long badheaderread = 0;
unsigned long tooshortread = 0;
unsigned long toolongread = 0;
unsigned long timeoutread = 0;
float readpercentage = 0;
static int uploadpercentage = 0;

// instead of passing array pointers between functions we just define this in the global scope
#define MAXDATASIZE 255
char data[MAXDATASIZE] = { '\0' };
byte data_length = 0;

// store actual data
String openTherm[2];
char actData[DATASIZE] = { '\0' };
char actDataExtra[DATASIZE] = { '\0' };
#define OPTDATASIZE 20
char actOptData[OPTDATASIZE]  = { '\0' };
String RESTmsg = "";

// log message to sprintf to
char log_msg[256];

// mqtt topic to sprintf and then publish to
char mqtt_topic[256];

static int mqttReconnects = 0;

// can't have too much in buffer due to memory shortage
#define MAXCOMMANDSINBUFFER 10

// buffer for commands to send
struct cmdbuffer_t {
  uint8_t length;
  byte data[128];
} cmdbuffer[MAXCOMMANDSINBUFFER];

static uint8_t cmdstart = 0;
static uint8_t cmdend = 0;
static uint8_t cmdnrel = 0;

// mqtt
WiFiClient mqtt_wifi_client;
PubSubClient mqtt_client(mqtt_wifi_client);

bool firstConnectSinceBoot = true; //if this is true there is no first connection made yet

struct timerqueue_t **timerqueue = NULL;
int timerqueue_size = 0;


/*
    check_wifi will process wifi reconnecting managing
*/
void check_wifi()
{
  if ((WiFi.status() != WL_CONNECTED) && (WiFi.localIP()))  {
    // special case where it seems that we are not connect but we do have working IP (causing the -1% wifi signal), do a reset.
    log_message(_F("Weird case, WiFi seems disconnected but is not. Resetting WiFi!"));
    setupWifi(&heishamonSettings);
  } else if ((WiFi.status() != WL_CONNECTED) || (!WiFi.localIP()))  {
    /* we need to stop reconnecting to a configured wifi network if there is a hotspot user connected
        also, do not disconnect if wifi network scan is active
    */
    if ((heishamonSettings.wifi_ssid[0] != '\0') && (WiFi.status() != WL_DISCONNECTED) && (WiFi.scanComplete() != -1) && (WiFi.softAPgetStationNum() > 0))  {
      log_message(_F("WiFi lost, but softAP station connecting, so stop trying to connect to configured ssid..."));
      WiFi.disconnect(true);
    }

    /*  only start this routine if timeout on
        reconnecting to AP and SSID is set
    */
    if ((heishamonSettings.wifi_ssid[0] != '\0') && ((unsigned long)(millis() - lastWifiRetryTimer) > WIFIRETRYTIMER ) )  {
      lastWifiRetryTimer = millis();
      if (WiFi.softAPSSID() == "") {
        log_message(_F("WiFi lost, starting setup hotspot..."));
        WiFi.softAP((char*)"HeishaMon-Setup");
      }
      if ((WiFi.status() == WL_DISCONNECTED)  && (WiFi.softAPgetStationNum() == 0 )) {
        log_message(_F("Retrying configured WiFi, ..."));
        if (heishamonSettings.wifi_password[0] == '\0') {
          WiFi.begin(heishamonSettings.wifi_ssid);
        } else {
          WiFi.begin(heishamonSettings.wifi_ssid, heishamonSettings.wifi_password);
        }
      } else {
        log_message(_F("Reconnecting to WiFi failed. Waiting a few seconds before trying again."));
        WiFi.disconnect(true);
      }
    }
  } else { //WiFi connected
    if (WiFi.softAPSSID() != "") {
      log_message(_F("WiFi (re)connected, shutting down hotspot..."));
      WiFi.softAPdisconnect(true);
    }

    if (firstConnectSinceBoot) { // this should start only when softap is down or else it will not work properly so run after the routine to disable softap
      firstConnectSinceBoot = false;
      lastMqttReconnectAttempt = 0; //initiate mqtt connection asap
      setupOTA();
    }

    /*
       always update if wifi is working so next time on ssid failure
       it only starts the routine above after this timeout
    */
    lastWifiRetryTimer = millis();
  }
}

void mqtt_reconnect()
{
  unsigned long now = millis();
  if ((lastMqttReconnectAttempt == 0) || ((unsigned long)(now - lastMqttReconnectAttempt) > MQTTRECONNECTTIMER)) { //only try reconnect each MQTTRECONNECTTIMER seconds or on boot when lastMqttReconnectAttempt is still 0
    lastMqttReconnectAttempt = now;
    log_message(_F("Reconnecting to mqtt server ..."));
    char topic[256];
    sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
    if (mqtt_client.connect(heishamonSettings.wifi_hostname, heishamonSettings.mqtt_username, heishamonSettings.mqtt_password, topic, 1, true, "Offline"))
    {
      mqttReconnects++;
      sprintf(topic, "%s/%s/#", heishamonSettings.mqtt_topic_base, mqtt_topic_commands);
      mqtt_client.subscribe(topic);
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_send_raw_value_topic);
      mqtt_client.subscribe(topic);
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
      mqtt_client.publish(topic, "Online");
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_iptopic);
      mqtt_client.publish(topic, WiFi.localIP().toString().c_str(), true);

      if (mqttReconnects == 1) { //only resend all data on first connect to mqtt so a data bomb like and bad mqtt server will not cause a reconnect bomb everytime
        resetlastalldatatime(); //resend all heatpump values to mqtt
      }
      //use this to receive valid heishamon raw data from other heishamon to debug this OT code
#define OTDEBUG
#ifdef OTDEBUG
      if ( heishamonSettings.listenonly && heishamonSettings.listenmqtt ) {
        sprintf(topic, "%s/raw/data", heishamonSettings.mqtt_topic_listen);
        mqtt_client.subscribe(topic); //subscribe to raw heatpump data over MQTT
      }
#endif
    }
  }
}

void log_message(char* string)
{
  time_t rawtime;
  rawtime = time(NULL);
  struct tm *timeinfo = localtime(&rawtime);
  char timestring[32];
  strftime(timestring, 32, "%c", timeinfo);
  size_t len = strlen(string) + strlen(timestring) + 20; //+20 long enough to contain millis()
  char* log_line = (char *) malloc(len);
  snprintf(log_line, len, "%s (%lu): %s", timestring, millis(), string);

  if (heishamonSettings.logSerial1) {
    Serial1.println(log_line);
  }
  if (heishamonSettings.logMqtt && mqtt_client.connected())
  {
    char log_topic[256];
    sprintf(log_topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_logtopic);

    if (!mqtt_client.publish(log_topic, log_line)) {
      if (heishamonSettings.logSerial1) {
        Serial1.print(millis());
        Serial1.print(F(": "));
        Serial1.println(F("MQTT publish log message failed!"));
      }
      mqtt_client.disconnect();
    }
  }
  websocket_write_all(log_line, strlen(log_line));
  free(log_line);
}

void logHex(char *hex, byte hex_len) {
#define LOGHEXBYTESPERLINE 32  // please be aware of max mqtt message size
  for (int i = 0; i < hex_len; i += LOGHEXBYTESPERLINE) {
    char buffer [(LOGHEXBYTESPERLINE * 3) + 1];
    buffer[LOGHEXBYTESPERLINE * 3] = '\0';
    for (int j = 0; ((j < LOGHEXBYTESPERLINE) && ((i + j) < hex_len)); j++) {
      sprintf(&buffer[3 * j], "%02X ", hex[i + j]);
    }
    sprintf_P(log_msg, PSTR("data: %s"), buffer ); log_message(log_msg);
  }
}

void mqttPublish(char* topic, char* subtopic, char* value) {
  char mqtt_topic[256];
  sprintf_P(mqtt_topic, PSTR("%s/%s/%s"), heishamonSettings.mqtt_topic_base, topic, subtopic);
  mqtt_client.publish(mqtt_topic, value, MQTT_RETAIN_VALUES);
}



byte calcChecksum(byte* command, int length) {
  byte chk = 0;
  for ( int i = 0; i < length; i++)  {
    chk += command[i];
  }
  chk = (chk ^ 0xFF) + 01;
  return chk;
}

bool isValidReceiveChecksum() {
  byte chk = 0;
  for ( int i = 0; i < data_length; i++)  {
    chk += data[i];
  }
  return (chk == 0); //all received bytes + checksum should result in 0
}

bool readSerial()
{
  int len = 0;
  while ((Serial.available()) && ((data_length + len) < MAXDATASIZE)) {
    data[data_length + len] = Serial.read(); //read available data and place it after the last received data
    len++;
    if (data[0] != 113) { //wrong header received!
      log_message(_F("Received bad header. Ignoring this data!"));
      if (heishamonSettings.logHexdump) logHex(data, len);
      badheaderread++;
      data_length = 0;
      return false; //return so this while loop does not loop forever if there happens to be a continous invalid data stream
    }
  }

  if ((len > 0) && (data_length == 0 )) totalreads++; //this is the start of a new read
  data_length += len;

  if (data_length > 1) { //should have received length part of header now

    if ((data_length > (data[1] + 3)) || (data_length >= MAXDATASIZE) ) {
      log_message(_F("Received more data than header suggests! Ignoring this as this is bad data."));
      if (heishamonSettings.logHexdump) logHex(data, data_length);
      data_length = 0;
      toolongread++;
      return false;
    }

    if (data_length == (data[1] + 3)) { //we received all data (data[1] is header length field)
      sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length); log_message(log_msg);
      sending = false; //we received an answer after our last command so from now on we can start a new send request again
      if (heishamonSettings.logHexdump) logHex(data, data_length);
      if (! isValidReceiveChecksum() ) {
        log_message(_F("Checksum received false!"));
        data_length = 0; //for next attempt
        badcrcread++;
        return false;
      }
      log_message(_F("Checksum and header received ok!"));
      goodreads++;

      if (data_length == DATASIZE)  {  //receive a full data block
        if  (data[3] == 0x10) { //decode the normal data block
          decode_heatpump_data(data, actData, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
          memcpy(actData, data, DATASIZE);
          {
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/data", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t *)actData, DATASIZE, false); //do not retain this raw data
          }
          data_length = 0;
          return true;
        } else if (data[3] == 0x21) { //decode the new model extra data block
          extraDataBlockAvailable = true; //set the flag to true so we know we can request this data always
          decode_heatpump_data_extra(data, actDataExtra, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
          memcpy(actDataExtra, data, DATASIZE);
          {
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/dataextra", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t *)actDataExtra, DATASIZE, false); //do not retain this raw data
          }
          data_length = 0;
          return true;        
        } else {
          log_message(_F("Received an unknown full size datagram. Can't decode this yet."));
          data_length = 0;
          return false;       
        }
      }
      else if (data_length == OPTDATASIZE ) { //optional pcb acknowledge answer
        log_message(_F("Received optional PCB ack answer. Decoding this in OPT topics."));
        decode_optional_heatpump_data(data, actOptData, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
        memcpy(actOptData, data, OPTDATASIZE);
        data_length = 0;
        return true;
      }
      else {
        log_message(_F("Received a shorter datagram. Can't decode this yet."));
        data_length = 0;
        return false;
      }
    }
  }
  return false;
}

void popCommandBuffer() {
  // to make sure we can pop a command from the buffer
  if ((!sending) && cmdnrel > 0) {
    send_command(cmdbuffer[cmdstart].data, cmdbuffer[cmdstart].length);
    cmdstart = (cmdstart + 1) % (MAXCOMMANDSINBUFFER);
    cmdnrel--;
  }
}

void pushCommandBuffer(byte* command, int length) {
  if (cmdnrel + 1 > MAXCOMMANDSINBUFFER) {
    log_message(_F("Too much commands already in buffer. Ignoring this commands.\n"));
    return;
  }
  cmdbuffer[cmdend].length = length;
  memcpy(&cmdbuffer[cmdend].data, command, length);
  cmdend = (cmdend + 1) % (MAXCOMMANDSINBUFFER);
  cmdnrel++;
}

bool send_command(byte* command, int length) {
  if ( heishamonSettings.listenonly ) {
    log_message(_F("Not sending this command. Heishamon in listen only mode!"));
    return false;
  }
  if ( sending ) {
    log_message(_F("Already sending data. Buffering this send request"));
    pushCommandBuffer(command, length);
    return false;
  }
  sending = true; //simple semaphore to only allow one send command at a time, semaphore ends when answered data is received

  byte chk = calcChecksum(command, length);
  int bytesSent = Serial.write(command, length); //first send command
  bytesSent += Serial.write(chk); //then calculcated checksum byte afterwards
  sprintf_P(log_msg, PSTR("sent bytes: %d including checksum value: %d "), bytesSent, int(chk));
  log_message(log_msg);

  if (heishamonSettings.logHexdump) logHex((char*)command, length);
  sendCommandReadTime = millis(); //set sendCommandReadTime when to timeout the answer of this command
  return true;
}

// Callback function that is called when a message has been pushed to one of your topics.
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (mqttcallbackinprogress) {
    log_message(_F("Already processing another mqtt callback. Ignoring this one"));
  }
  else {
    mqttcallbackinprogress = true; //simple semaphore to make sure we don't have two callbacks at the same time
    char msg[length + 1];
    for (unsigned int i = 0; i < length; i++) {
      msg[i] = (char)payload[i];
    }
    msg[length] = '\0';
    char* topic_command = topic + strlen(heishamonSettings.mqtt_topic_base) + 1; //strip base plus seperator from topic
    if (strcmp(topic_command, mqtt_send_raw_value_topic) == 0)
    { // send a raw hex string
      byte *rawcommand;
      rawcommand = (byte *) malloc(length);
      memcpy(rawcommand, msg, length);

      sprintf_P(log_msg, PSTR("sending raw value"));
      log_message(log_msg);
      send_command(rawcommand, length);
      free(rawcommand);
    } else if (strncmp(topic_command, mqtt_topic_commands, strlen(mqtt_topic_commands)) == 0)  // check for commands to heishamon
    {
      char* topic_sendcommand = topic_command + strlen(mqtt_topic_commands) + 1; //strip the first 9 "commands/" from the topic to get what we need
      send_heatpump_command(topic_sendcommand, msg, send_command, log_message, heishamonSettings.optionalPCB);
    }
    //use this to receive valid heishamon raw data from other heishamon to debug this OT code
#ifdef OTDEBUG
    else if (strcmp((char*)"panasonic_heat_pump/data", topic) == 0) {  // check for raw heatpump input
      sprintf_P(log_msg, PSTR("Received raw heatpump data from MQTT"));
      log_message(log_msg);
      decode_heatpump_data(msg, actData, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
      memcpy(actData, msg, DATASIZE);
#endif
    }
    mqttcallbackinprogress = false;
  }
}

void setupOTA() {
  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(heishamonSettings.wifi_hostname);

  // Set authentication
  ArduinoOTA.setPassword(heishamonSettings.ota_password);

  ArduinoOTA.onStart([]() {
  });
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {

  });
  ArduinoOTA.onError([](ota_error_t error) {

  });
  ArduinoOTA.begin();
}

void setupSerial() {
  //boot issue's first on normal serial
  // Serial.begin(115200);
  // Serial.flush();
}

void setupSerial1() {
  // if (heishamonSettings.logSerial1) { //settings are not loaded yet, this is the startup default
  //   //debug line on serial1 (D4, GPIO2)
  //   Serial1.begin(115200);
  //   Serial1.print(F("Starting debugging, version: "));
  //   Serial1.println(heishamon_version);
  // }
  // else {
  //   pinMode(2, FUNCTION_0); //set it as gpio
  // }
}

void switchSerial() {
  // Serial.println(F("Switching serial to connect to heatpump. Look for debug on serial1 (GPIO2) and mqtt log topic."));
  // //serial to cn-cnt
  // Serial.flush();
  // Serial.end();
  // Serial.begin(9600, SERIAL_8E1);
  // Serial.flush();
  // //turn on GPIO's on tx/rx for opentherm part
  // pinMode(1, FUNCTION_3);
  // pinMode(3, FUNCTION_3);

  // setupGPIO(heishamonSettings.gpioSettings); //switch extra GPIOs to configured mode

  // //mosfet output enable
  // pinMode(5, OUTPUT);

  // //try to detect if cz-taw1 is connected in parallel
  // if (!heishamonSettings.listenonly) {
  //   if (Serial.available() > 0) {
  //     log_message(_F("There is data on the line without asking for it. Switching to listen only mode."));
  //     heishamonSettings.listenonly = true;
  //   }
  //   else {
  //     //enable gpio15 after boot using gpio5 (D1) which enables the level shifter for the tx to panasonic
  //     //do not enable if listen only to keep the line floating
  //     digitalWrite(5, HIGH);
  //   }
  // }
}

void setupMqtt() {
  mqtt_client.setBufferSize(1024);
  mqtt_client.setSocketTimeout(10); mqtt_client.setKeepAlive(5); //fast timeout, any slower will block the main loop too long
  mqtt_client.setServer(heishamonSettings.mqtt_server, atoi(heishamonSettings.mqtt_port));
  mqtt_client.setCallback(mqtt_callback);
}

void setupConditionals() {
  //send_initial_query(); //maybe necessary but for now disable. CZ-TAW1 sends this query on boot

  //load optional PCB data from flash
  if (heishamonSettings.optionalPCB) {
    if (loadOptionalPCB(optionalPCBQuery, OPTIONALPCBQUERYSIZE)) {
      log_message(_F("Succesfully loaded optional PCB data from saved flash!"));
    }
    else {
      log_message(_F("Failed to load optional PCB data from flash!"));
    }
    delay(1500); //need 1.5 sec delay before sending first datagram
    send_optionalpcb_query(); //send one datagram already at start
    lastOptionalPCBRunTime = millis();
  }
}


void timer_cb(int nr) {
  // if (nr > 0) {
  //   rules_timer_cb(nr);
  // } else {
  //   switch (nr) {
  //     case -1: {
  //         LittleFS.begin();
  //         LittleFS.format();
  //         //create first boot file
  //         File startupFile = LittleFS.open("/heishamon", "w");
  //         startupFile.close(); 
  //         WiFi.disconnect(true);
  //         timerqueue_insert(1, 0, -2);
  //       } break;
  //     case -2: {
  //         ESP.restart();
  //       } break;
  //     case -3: {
  //         setupWifi(&heishamonSettings);
  //       } break;
  //     case -4: {
  //         if (rules_parse("/rules.new") == -1) {
  //           logprintln_P(F("new ruleset failed to parse, using previous ruleset"));
  //           rules_parse("/rules.txt");
  //         } else {
  //           logprintln_P(F("new ruleset successfully parsed"));
  //           if (LittleFS.begin()) {
  //             LittleFS.rename("/rules.new", "/rules.txt");
  //           }
  //         }
  //         rules_boot();
  //       } break;
  //   }
  // }

}


void setup() {
  //first get total memory before we do anything
  getFreeMemory();

  setupSerial();
  setupSerial1();

  Serial.println();
  Serial.println(F("--- HEISHAMON ---"));
  Serial.println(F("starting..."));

  WiFi.printDiag(Serial);
  //initiate a wifi scan at boot to prefill the wifi scan json list
  byte numSsid = WiFi.scanNetworks();

  loadSettings(&heishamonSettings);

  setupWifi(&heishamonSettings);

  setupMqtt();
  // setupHttp();

  switchSerial(); //switch serial to gpio13/gpio15
  WiFi.printDiag(Serial1);

  setupConditionals(); //setup for routines based on settings
}

void send_initial_query() {
  log_message(_F("Requesting initial start query"));
  send_command(initialQuery, INITIALQUERYSIZE);

}

void send_panasonic_query() {
  log_message(_F("Requesting new panasonic data"));
  send_command(panasonicQuery, PANASONICQUERYSIZE);
  // rest is for the new data block on new models
  if (extraDataBlockAvailable) {
    log_message(_F("Requesting new panasonic extra data"));
    panasonicQuery[3] = 0x21; //setting 4th byte to 0x21 is a request for extra block
    send_command(panasonicQuery, PANASONICQUERYSIZE);
    panasonicQuery[3] = 0x10; //setting 4th back to 0x10 for normal data request next time
  } else if (!extraDataBlockChecked) {
    if ((actData[0] == 0x71) && (actData[193] == 0) ) { //do we have data but 0 value in heat consumptiom power, then assume K or L series
      extraDataBlockChecked = true;
      log_message(_F("Checking if connected heatpump has extra data"));
      panasonicQuery[3] = 0x21;
      send_command(panasonicQuery, PANASONICQUERYSIZE);
      panasonicQuery[3] = 0x10;   
    }
  }
}

void send_optionalpcb_query() {
  log_message(_F("Sending optional PCB data"));
  send_command(optionalPCBQuery, OPTIONALPCBQUERYSIZE);
}


void read_panasonic_data() {
  if (sending && ((unsigned long)(millis() - sendCommandReadTime) > SERIALTIMEOUT)) {
    log_message(_F("Previous read data attempt failed due to timeout!"));
    sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length);
    log_message(log_msg);
    if (heishamonSettings.logHexdump) logHex(data, data_length);
    if (data_length == 0) {
      timeoutread++;
      totalreads++; //at at timeout we didn't receive anything but did expect it so need to increase this for the stats
    } else {
      tooshortread++;
    }
    data_length = 0; //clear any data in array
    sending = false; //receiving the answer from the send command timed out, so we are allowed to send a new command
  }
  if ( (heishamonSettings.listenonly || sending) && (Serial.available() > 0)) readSerial();
}

void loop() {
  webserver_loop();

  // check wifi
  check_wifi();
  // Handle OTA first.
  ArduinoOTA.handle();

  mqtt_client.loop();

  read_panasonic_data();

  if ((!sending) && (cmdnrel > 0)) { //check if there is a send command in the buffer
    log_message(_F("Sending command from buffer"));
    popCommandBuffer();
  }

  if ((!sending) && (!heishamonSettings.listenonly) && (heishamonSettings.optionalPCB) && ((unsigned long)(millis() - lastOptionalPCBRunTime) > OPTIONALPCBQUERYTIME) ) {
    lastOptionalPCBRunTime = millis();
    send_optionalpcb_query();
    if ((unsigned long)(millis() - lastOptionalPCBSave) > (1000 * OPTIONALPCBSAVETIME)) {  // only save each 5 minutes
      lastOptionalPCBSave = millis();
      if (saveOptionalPCB(optionalPCBQuery, OPTIONALPCBQUERYSIZE)) {
        log_message((char*)"Succesfully saved optional PCB data to flash!");
      } else {
        log_message((char*)"Failed to save optional PCB data to flash!");
      }
    }
  }

  // run the data query only each WAITTIME
  if ((unsigned long)(millis() - lastRunTime) > (1000 * heishamonSettings.waitTime)) {
    lastRunTime = millis();
    //check mqtt
    if ( (WiFi.isConnected()) && (!mqtt_client.connected()) )
    {
      log_message(_F("Lost MQTT connection!"));
      mqtt_reconnect();
    }

    //log stats
    if (totalreads > 0 ) readpercentage = (((float)goodreads / (float)totalreads) * 100);
    String message;
    message.reserve(384);
    message += F("Heishamon stats: Uptime: ");
    message += F(" ## Free memory: ");
    message += getFreeMemory();
    message += F(" bytes ## Wifi: ");
    message += getWifiQuality();
    message += F("% (RSSI: ");
    message += WiFi.RSSI();
    message += F(") ## Mqtt reconnects: ");
    message += mqttReconnects;
    message += F(" ## Correct data: ");
    message += readpercentage;
    message += F("%");
    log_message((char*)message.c_str());

    String stats;
    stats.reserve(384);
    stats += F("{\"uptime\":");
    stats += String(millis());
    // ESP32:Disabled
    // stats += F(",\"voltage\":");
    // stats += ESP.getVcc() / 1024.0;
    stats += F(",\"free memory\":");
    stats += getFreeMemory();
    stats += F(",\"free heap\":");
    stats += ESP.getFreeHeap();
    stats += F(",\"wifi\":");
    stats += getWifiQuality();
    stats += F(",\"mqtt reconnects\":");
    stats += mqttReconnects;
    stats += F(",\"total reads\":");
    stats += totalreads;
    stats += F(",\"good reads\":");
    stats += goodreads;
    stats += F(",\"bad crc reads\":");
    stats += badcrcread;
    stats += F(",\"bad header reads\":");
    stats += badheaderread;
    stats += F(",\"too short reads\":");
    stats += tooshortread;
    stats += F(",\"too long reads\":");
    stats += toolongread;
    stats += F(",\"timeout reads\":");
    stats += timeoutread;
    stats += F(",\"version\":\"");
    stats += heishamon_version;
    stats += F("\"}");
    sprintf_P(mqtt_topic, PSTR("%s/stats"), heishamonSettings.mqtt_topic_base);
    mqtt_client.publish(mqtt_topic, stats.c_str(), MQTT_RETAIN_VALUES);

    //get new data
    if (!heishamonSettings.listenonly) send_panasonic_query();

    //Make sure the LWT is set to Online, even if the broker have marked it dead.
    sprintf_P(mqtt_topic, PSTR("%s/%s"), heishamonSettings.mqtt_topic_base, mqtt_willtopic);
    mqtt_client.publish(mqtt_topic, "Online");
  }

  timerqueue_update();
}

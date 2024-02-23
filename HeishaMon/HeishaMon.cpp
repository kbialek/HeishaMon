
#define LWIP_INTERNAL

#include <WiFi.h>
#include <WiFiUdp.h>
#define NO_OTA_PORT
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <HardwareSerial.h>

#include "commands.h"
#include "decode.h"
#include "src/common/log.h"
#include "src/common/progmem.h"
#include "src/common/stricmp.h"
#include "version.h"
#include "webfunctions.h"

#define SERIALTIMEOUT 2000  // wait until all 203 bytes are read, must not be too long to avoid blocking the code

settingsStruct heishamonSettings;

bool sending = false;                 // mutex for sending data
bool mqttcallbackinprogress = false;  // mutex for processing mqtt callback

bool extraDataBlockAvailable = false;  // this will be set to true if, during boot, heishamon detects this heatpump has extra data block (like K and L series do)
bool extraDataBlockChecked = false;    // this will be true if we already checked for the extra data block

#define MQTTRECONNECTTIMER 30000  // it takes 30 secs for each mqtt server reconnect attempt
unsigned long lastMqttReconnectAttempt = 0;

#define WIFIRETRYTIMER 15000  // switch between hotspot and configured SSID each 10 secs if SSID is lost
unsigned long lastWifiRetryTimer = 0;

unsigned long lastRunTime = 0;
unsigned long lastSendValveCommandRunTime = 0;
unsigned long lastOptionalPCBRunTime = 0;
unsigned long lastOptionalPCBSave = 0;

unsigned long sendCommandReadTime = 0;  // set to millis value during send, allow to wait millis for answer
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
char data[MAXDATASIZE] = {'\0'};
byte data_length = 0;

char cztaw_data[MAXDATASIZE] = {'\0'};
byte cztaw_data_length = 0;

#ifdef LOG_HPDUMP
char raw_data[MAXDATASIZE] = {'\0'};
byte raw_data_length = 0;
#endif

// store actual data
char actData[DATASIZE] = {'\0'};
char actDataExtra[DATASIZE] = {'\0'};
#define OPTDATASIZE 20
char actOptData[OPTDATASIZE] = {'\0'};
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

bool firstConnectSinceBoot = true;  // if this is true there is no first connection made yet

struct timerqueue_t** timerqueue = NULL;
int timerqueue_size = 0;

void log_message(char* string) {
    time_t rawtime;
    rawtime = time(NULL);
    struct tm* timeinfo = localtime(&rawtime);
    char timestring[32];
    strftime(timestring, 32, "%c", timeinfo);
    size_t len = strlen(string) + strlen(timestring) + 20;  //+20 long enough to contain millis()
    char* log_line = (char*)malloc(len);
    snprintf(log_line, len, "%s (%lu): %s", timestring, millis(), string);

    logprintln(log_line);
    if (heishamonSettings.logMqtt && mqtt_client.connected()) {
        char log_topic[256];
        sprintf(log_topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_logtopic);

        if (!mqtt_client.publish(log_topic, log_line)) {
            Serial.print(millis());
            Serial.print(F(": "));
            Serial.println(F("MQTT publish log message failed!"));
            mqtt_client.disconnect();
        }
    }
    free(log_line);
}

void mqttPublish(char* topic, char* subtopic, char* value) {
    char mqtt_topic[256];
    sprintf_P(mqtt_topic, PSTR("%s/%s/%s"), heishamonSettings.mqtt_topic_base, topic, subtopic);
    mqtt_client.publish(mqtt_topic, value, MQTT_RETAIN_VALUES);
}

byte calcChecksum(byte* command, int length) {
    byte chk = 0;
    for (int i = 0; i < length; i++) {
        chk += command[i];
    }
    chk = (chk ^ 0xFF) + 01;
    return chk;
}

bool isValidReceiveChecksum() {
    byte chk = 0;
    for (int i = 0; i < data_length; i++) {
        chk += data[i];
    }
    return (chk == 0);  // all received bytes + checksum should result in 0
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
    if (sending) {
        log_message(_F("Already sending data. Buffering this send request"));
        pushCommandBuffer(command, length);
        return false;
    }
    sending = true;  // simple semaphore to only allow one send command at a time, semaphore ends when answered data is received

    byte chk = calcChecksum(command, length);
    int bytesSent = Serial1.write(command, length);  // first send command
    bytesSent += Serial1.write(chk);                 // then calculcated checksum byte afterwards
    sprintf_P(log_msg, PSTR("sent bytes: %d including checksum value: %d "), bytesSent, int(chk));
    log_message(log_msg);

    sendCommandReadTime = millis();  // set sendCommandReadTime when to timeout the answer of this command
    return true;
}

void popCommandBuffer() {
    // to make sure we can pop a command from the buffer
    if ((!sending) && cmdnrel > 0) {
        send_command(cmdbuffer[cmdstart].data, cmdbuffer[cmdstart].length);
        cmdstart = (cmdstart + 1) % (MAXCOMMANDSINBUFFER);
        cmdnrel--;
    }
}

bool readCzTawSerial() {
    int len = 0;

    while (Serial2.available()) {
        int byte_from_cztaw = Serial2.read();
        if ((cztaw_data_length + len) < MAXDATASIZE) {
            cztaw_data[cztaw_data_length + len] = byte_from_cztaw;
            len++;
        }
        if (cztaw_data[0] != 0x31 && cztaw_data[0] != 0x71 && cztaw_data[0] != 0xf1) {  // wrong cz-taw query header received
            log_message(_F("Received bad header from CZ-TAW. Ignoring this data!"));
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/cztaw", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t*)cztaw_data, cztaw_data_length, false);  // do not retain this raw data
            cztaw_data_length = 0;
            len = 0;
            return false;
        }
    }
    cztaw_data_length += len;

    if (cztaw_data_length > 1) {
        if ((cztaw_data_length > (cztaw_data[1] + 3)) || (cztaw_data_length >= MAXDATASIZE)) {
            log_message(_F("Received more data than CZ-TAW header suggests! Ignoring this as this is bad data."));
            cztaw_data_length = 0;
            return false;
        }
        if (cztaw_data_length == (cztaw_data[1] + 3)) {
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/cztaw", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t*)cztaw_data, cztaw_data_length, false);  // do not retain this raw data
            return true;
        }
    }

    return false;
}

void czTawLoop() {
    if (Serial2.available() == 0 || !readCzTawSerial()) {
        return;
    }

    if (cztaw_data[0] != 0x71) {
        // non-query commands are sent to the heatpump
        send_command((byte*)cztaw_data, cztaw_data_length - 1);  // skip the checksum, as it will be recalculated when sending the command
    }

    if ((cztaw_data[0] == 0x71 || cztaw_data[0] == 0xf1) && cztaw_data[3] == 0x10) {
        if (actData[0] == 0x71) {  // send heatpump state to cz-taw only if it's already read from the heatpump
            log_message(_F("Responding to heatpump data frame from CZ-TAW"));
            Serial2.write(actData, DATASIZE);
            Serial2.flush(true);
        }
    }

    cztaw_data_length = 0;
}

bool readHeatpumpSerial() {
    int len = 0;

#ifdef LOG_HPDUMP
    char raw_data_mqtt_topic[256];
    sprintf(raw_data_mqtt_topic, "%s/raw/hpdump", heishamonSettings.mqtt_topic_base);
#endif

    while ((Serial1.available()) && ((data_length + len) < MAXDATASIZE)) {
        int byte_from_hp = Serial1.read();
        data[data_length + len] = byte_from_hp;  // read available data and place it after the last received data
#ifdef LOG_HPDUMP
        {
            raw_data[raw_data_length++] = byte_from_hp;
            if (raw_data_length == 32) {
                mqtt_client.publish(raw_data_mqtt_topic, (const uint8_t*)raw_data, 32, false);
                raw_data_length = 0;
            }
        }
#endif
        len++;
        if (data[0] != 0x31 && data[0] != 0x71) {  // wrong header received!
            log_message(_F("Received bad header from HP. Ignoring this data!"));
            badheaderread++;
            data_length = 0;
            return false;  // return so this while loop does not loop forever if there happens to be a continous invalid data stream
        }
    }

    if ((len > 0) && (data_length == 0)) totalreads++;  // this is the start of a new read
    data_length += len;

    if (data_length > 1) {  // should have received length part of header now

        if ((data_length > (data[1] + 3)) || (data_length >= MAXDATASIZE)) {
            log_message(_F("Received more data than header suggests! Ignoring this as this is bad data."));
            data_length = 0;
            toolongread++;
            return false;
        }

        if (data_length == (data[1] + 3)) {  // we received all data (data[1] is header length field)
#ifdef LOG_HPDUMP
            if (raw_data_length > 0) {
                mqtt_client.publish(raw_data_mqtt_topic, (const uint8_t*)raw_data, raw_data_length, false);
                raw_data_length = 0;
            }
#endif

            sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length);
            log_message(log_msg);
            sending = false;  // we received an answer after our last command so from now on we can start a new send request again
            if (!isValidReceiveChecksum()) {
                log_message(_F("Checksum received false!"));
                data_length = 0;  // for next attempt
                badcrcread++;
                return false;
            }
            log_message(_F("Checksum and header received ok!"));
            goodreads++;

            if (data[0] == 0x31) {
                // forward the handshake? data block
                {
                    char mqtt_topic[256];
                    sprintf(mqtt_topic, "%s/raw/hp", heishamonSettings.mqtt_topic_base);
                    mqtt_client.publish(mqtt_topic, (const uint8_t*)data, data_length, false);
                }
                Serial2.write(data, data_length);
                Serial2.flush(true);
                data_length = 0;
                return true;
            } else if (data_length == DATASIZE) {  // receive a full data block
                if (data[3] == 0x10) {             // decode the normal data block
                    decode_heatpump_data(data, actData, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
                    memcpy(actData, data, DATASIZE);
                    {
                        char mqtt_topic[256];
                        sprintf(mqtt_topic, "%s/raw/hp", heishamonSettings.mqtt_topic_base);
                        mqtt_client.publish(mqtt_topic, (const uint8_t*)actData, DATASIZE, false);  // do not retain this raw data
                    }
                    data_length = 0;
                    return true;
                } else if (data[3] == 0x21) {        // decode the new model extra data block
                    extraDataBlockAvailable = true;  // set the flag to true so we know we can request this data always
                    decode_heatpump_data_extra(data, actDataExtra, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
                    memcpy(actDataExtra, data, DATASIZE);
                    {
                        char mqtt_topic[256];
                        sprintf(mqtt_topic, "%s/raw/dataextra", heishamonSettings.mqtt_topic_base);
                        mqtt_client.publish(mqtt_topic, (const uint8_t*)actDataExtra, DATASIZE, false);  // do not retain this raw data
                    }
                    data_length = 0;
                    return true;
                } else {
                    log_message(_F("Received an unknown full size datagram. Can't decode this yet."));
                    data_length = 0;
                    return false;
                }
            } else if (data_length == OPTDATASIZE) {  // optional pcb acknowledge answer
                log_message(_F("Received optional PCB ack answer. Decoding this in OPT topics."));
                decode_optional_heatpump_data(data, actOptData, mqtt_client, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
                memcpy(actOptData, data, OPTDATASIZE);
                {
                    char mqtt_topic[256];
                    sprintf(mqtt_topic, "%s/raw/hpopt", heishamonSettings.mqtt_topic_base);
                    mqtt_client.publish(mqtt_topic, (const uint8_t*)actOptData, OPTDATASIZE, false);  // do not retain this raw data
                }
                data_length = 0;
                return true;
            } else {
                log_message(_F("Received a shorter datagram. Can't decode this yet."));
                data_length = 0;
                return false;
            }
        }
    }
    return false;
}

// Callback function that is called when a message has been pushed to one of your topics.
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (mqttcallbackinprogress) {
        log_message(_F("Already processing another mqtt callback. Ignoring this one"));
    } else {
        mqttcallbackinprogress = true;  // simple semaphore to make sure we don't have two callbacks at the same time
        char msg[length + 1];
        for (unsigned int i = 0; i < length; i++) {
            msg[i] = (char)payload[i];
        }
        msg[length] = '\0';
        char* topic_command = topic + strlen(heishamonSettings.mqtt_topic_base) + 1;  // strip base plus seperator from topic
        if (strcmp(topic_command, mqtt_send_raw_value_topic) == 0) {                  // send a raw hex string
            byte* rawcommand;
            rawcommand = (byte*)malloc(length);
            memcpy(rawcommand, msg, length);

            sprintf_P(log_msg, PSTR("sending raw value"));
            log_message(log_msg);
            send_command(rawcommand, length);
            free(rawcommand);
        } else if (strncmp(topic_command, mqtt_topic_commands, strlen(mqtt_topic_commands)) == 0)  // check for commands to heishamon
        {
            char* topic_sendcommand = topic_command + strlen(mqtt_topic_commands) + 1;  // strip the first 9 "commands/" from the topic to get what we need
            send_heatpump_command(topic_sendcommand, msg, send_command, log_message, heishamonSettings.optionalPCB);
        }
        mqttcallbackinprogress = false;
    }
}

void connect_mqtt() {
    if (!WiFi.isConnected() || mqtt_client.connected()) {
        return;
    }

    unsigned long now = millis();
    if ((lastMqttReconnectAttempt == 0) || ((unsigned long)(now - lastMqttReconnectAttempt) > MQTTRECONNECTTIMER)) {  // only try reconnect each MQTTRECONNECTTIMER seconds or on boot when lastMqttReconnectAttempt is still 0
        lastMqttReconnectAttempt = now;
        log_message(_F("Reconnecting to mqtt server ..."));
        char topic[256];
        sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
        if (mqtt_client.connect("heishamon", MQTT_USERNAME, MQTT_PASSWORD, topic, 1, true, "Offline")) {
            mqttReconnects++;
            sprintf(topic, "%s/%s/#", heishamonSettings.mqtt_topic_base, mqtt_topic_commands);
            mqtt_client.subscribe(topic);
            sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_send_raw_value_topic);
            mqtt_client.subscribe(topic);
            sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
            mqtt_client.publish(topic, "Online");
            sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_iptopic);
            mqtt_client.publish(topic, WiFi.localIP().toString().c_str(), true);

            if (mqttReconnects == 1) {   // only resend all data on first connect to mqtt so a data bomb like and bad mqtt server will not cause a reconnect bomb everytime
                resetlastalldatatime();  // resend all heatpump values to mqtt
            }
        }
    }
}

void connect() {
    connect_mqtt();
}

void setupOTA() {
    ArduinoOTA.onStart([]() {
        Serial.println("OTA update started");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA update finished");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
            Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
            Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
            Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
            Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
            Serial.println("End Failed");
    });
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.begin();
    Serial.println("Ready for OTA updates");
}

void setupSerial() {
    // logger port
    Serial.begin(115200);
    Serial.flush();

    // Heatpump Connection
    // RX Pin -> GPIO 18 (D18)
    // TX Pin -> GPIO 19 (D19)
    Serial1.begin(9600, SERIAL_8E1, 18, 19);
    Serial1.flush();

    // CZ-TAW Connection
    // RX Pin -> GPIO 16 (RX2)
    // TX Pin -> GPIO 17 (TX2)
    Serial2.begin(9600, SERIAL_8E1);
    Serial2.flush();
}

void setupWifi() {
    WiFi.mode(WIFI_STA);
    IPAddress ip, gateway, subnet;
    ip.fromString(WIFI_ADDRESS);
    gateway.fromString(WIFI_GATEWAY);
    subnet.fromString(WIFI_SUBNET);
    if (!WiFi.config(ip, gateway, subnet, IPAddress(), IPAddress())) {
        Serial.println("STA Failed to configure");
        return;
    }
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.printDiag(Serial);
}

void setupMqtt() {
    mqtt_client.setBufferSize(1024);
    mqtt_client.setSocketTimeout(10);
    mqtt_client.setKeepAlive(5);  // fast timeout, any slower will block the main loop too long
    mqtt_client.setServer(MQTT_SERVER, atoi(heishamonSettings.mqtt_port));
    mqtt_client.setCallback(mqtt_callback);
}

void send_optionalpcb_query() {
    log_message(_F("Sending optional PCB data"));
    send_command(optionalPCBQuery, OPTIONALPCBQUERYSIZE);
}

void setup() {
    // first get total memory before we do anything
    getFreeMemory();

    setupSerial();

    Serial.println();
    Serial.println(F("--- HEISHAMON ---"));
    Serial.println(F("starting..."));

    setupWifi();
    setupMqtt();
    setupOTA();

    connect();

    lastSendValveCommandRunTime = millis() + 15 * 1000;
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
        panasonicQuery[3] = 0x21;  // setting 4th byte to 0x21 is a request for extra block
        send_command(panasonicQuery, PANASONICQUERYSIZE);
        panasonicQuery[3] = 0x10;  // setting 4th back to 0x10 for normal data request next time
    } else if (!extraDataBlockChecked) {
        if ((actData[0] == 0x71) && (actData[193] == 0)) {  // do we have data but 0 value in heat consumptiom power, then assume K or L series
            extraDataBlockChecked = true;
            log_message(_F("Checking if connected heatpump has extra data"));
            panasonicQuery[3] = 0x21;
            send_command(panasonicQuery, PANASONICQUERYSIZE);
            panasonicQuery[3] = 0x10;
        }
    }
}

void read_panasonic_data() {
    if (sending && ((unsigned long)(millis() - sendCommandReadTime) > SERIALTIMEOUT)) {
        log_message(_F("Previous read data attempt failed due to timeout!"));
        sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length);
        log_message(log_msg);
        if (data_length == 0) {
            timeoutread++;
            totalreads++;  // at at timeout we didn't receive anything but did expect it so need to increase this for the stats
        } else {
            tooshortread++;
        }
        data_length = 0;  // clear any data in array
        sending = false;  // receiving the answer from the send command timed out, so we are allowed to send a new command
    }

    if (Serial1.available() > 0) {
        readHeatpumpSerial();
    }
}

void loop() {
    connect();

    // Handle OTA first.
    ArduinoOTA.handle();

    mqtt_client.loop();

    czTawLoop();

    read_panasonic_data();

    if ((!sending) && (cmdnrel > 0)) {  // check if there is a send command in the buffer
        log_message(_F("Sending command from buffer"));
        popCommandBuffer();
    }

    if ((!sending) && (heishamonSettings.optionalPCB) && ((unsigned long)(millis() - lastOptionalPCBRunTime) > OPTIONALPCBQUERYTIME)) {
        lastOptionalPCBRunTime = millis();
        send_optionalpcb_query();
    }

    if ((unsigned long)(millis() - lastSendValveCommandRunTime) > (1000 * 20)) {
        lastSendValveCommandRunTime = millis();
        log_message(_F("Setting Z1/Z2 water temp"));
        char msg[16] = { 0 };

        String mainOutletTemp = getDataValue(actData, 6 /* Main_Outlet_Temp */);
        strcpy(msg, mainOutletTemp.c_str());
        if (atoi(msg) < 10) {
            strcpy(msg, "10");
        }

        send_heatpump_command((char*)"SetZ1WaterTemp", msg, send_command, log_message, true);
        send_heatpump_command((char*)"SetZ2WaterTemp", msg, send_command, log_message, true);
    }

    // run the data query only each WAITTIME
    if ((unsigned long)(millis() - lastRunTime) > (1000 * heishamonSettings.waitTime)) {
        lastRunTime = millis();
        // log stats
        if (totalreads > 0) readpercentage = (((float)goodreads / (float)totalreads) * 100);
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

        // get new data
        send_panasonic_query();

        // Make sure the LWT is set to Online, even if the broker have marked it dead.
        sprintf_P(mqtt_topic, PSTR("%s/%s"), heishamonSettings.mqtt_topic_base, mqtt_willtopic);
        mqtt_client.publish(mqtt_topic, "Online");
    }
}

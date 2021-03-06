/*
 */
#include <memory>
#include <cstring>
#include <vector>

#include "makestring.h"
#include "crceeprom.h"

#include <ESP8266WiFi.h>  // https://github.com/esp8266/Arduino
#include <ESP8266mDNS.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include "LittleFS.h"

#include <propertyutils.h>
#include <optparser.hpp>
#include <utils.h>

#include <PubSubClient.h> // https://github.com/knolleary/pubsubclient/releases/tag/v2.6

#include <config.h>
#include <digitalknob.h>

#include <statemachine.h>

typedef PropertyValue PV ;


// Number calls per second we will be handling
#define FRAMES_PER_SECOND        50
#define EFFECT_PERIOD_CALLBACK   (1000 / FRAMES_PER_SECOND)

// of transitions
volatile uint32_t counter50TimesSec = 1;

// Keep track when the last time we ran the effect state changes
uint32_t effectPeriodStartMillis = 0;

// start time when the bell starting ringing
uint32_t bellStartTime = 0;

// Analog and digital inputs
DigitalKnob digitalKnob(BUTTON_PIN, INVERT_INPUT, 110);

// Stores information about the bell
Properties controllerConfig;
volatile bool controllerConfigModified = false;

// CRC value of last update to MQTT
volatile uint16_t lastMeasurementCRC = 0;

// Indicate that a service requested an restart. Set to millies() of current time and it will restart 5000ms later
volatile uint32_t shouldRestart = 0;

// MQTT Status stuff
volatile bool hasMqttConfigured = false;
char* mqttSubscriberTopic;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// State machine states and configurations
std::unique_ptr<StateMachine> bootSequence(nullptr);

// WM configuration
WiFiManager wm;
#define MQTT_SERVER_LENGTH 40
#define MQTT_PORT_LENGTH 5
#define MQTT_USERNAME_LENGTH 18
#define MQTT_PASSWORD_LENGTH 18
WiFiManagerParameter wm_mqtt_server("server", "mqtt server", "", MQTT_SERVER_LENGTH);
WiFiManagerParameter wm_mqtt_port("port", "mqtt port", "", MQTT_PORT_LENGTH);
WiFiManagerParameter wm_mqtt_user("user", "mqtt username", "", MQTT_USERNAME_LENGTH);
const char _customHtml_hidden[] = "type=\"password\"";
WiFiManagerParameter wm_mqtt_password("input", "mqtt password", "", MQTT_PASSWORD_LENGTH, _customHtml_hidden, WFM_LABEL_AFTER);


///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////
//  LittleFS
///////////////////////////////////////////////////////////////////////////


bool loadConfig(const char* filename, Properties& properties) {
    bool ret = false;

    if (LittleFS.begin()) {
        Serial.println("mounted file system");

        if (LittleFS.exists(filename)) {
            //file exists, reading and loading
            File configFile = LittleFS.open(filename, "r");

            if (configFile) {
                Serial.print(F("Loading config : "));
                Serial.println(filename);
                deserializeProperties<32>(configFile, properties);
                //   serializeProperties<32>(Serial, properties);
            }

            configFile.close();
        } else {
            Serial.print(F("File not found: "));
            Serial.println(filename);
        }

        // LittleFS.end();
    } else {
        Serial.print(F("Failed to begin LittleFS"));
    }

    return ret;
}


/**
 * Store custom oarameter configuration in LittleFS
 */
bool saveConfig(const char* filename, Properties& properties) {
    bool ret = false;

    if (LittleFS.begin()) {
        LittleFS.remove(filename);
        File configFile = LittleFS.open(filename, "w");

        if (configFile) {
            Serial.print(F("Saving config : "));
            Serial.println(filename);
            serializeProperties<32>(configFile, properties);
            // serializeProperties<32>(Serial, properties);
            ret = true;
        } else {
            Serial.print(F("Failed to write file"));
            Serial.println(filename);
        }

        configFile.close();
        //    LittleFS.end();
    }

    return ret;
}

///////////////////////////////////////////////////////////////////////////
//  MQTT
///////////////////////////////////////////////////////////////////////////
/*
* Publish current status
* en = Ringer is enabled
* ri = When bool is pressed
*
*/
void publishRelativeToBaseMQTT(const char* topic, const char* payload);
void publishStatusToMqtt() {

    auto format = "en=%i ri=%i";
    char buffer[16]; // 10 characters per item times extra items to be sure
    bool button = digitalKnob.current();
    boolean ringerOn = controllerConfig.get("ringerOn");
    sprintf(buffer, format,
            ringerOn,
            button
           );

    // Quick hack to only update when data actually changed
    uint16_t thisCrc = CRCEEProm::crc16((uint8_t*)buffer, std::strlen(buffer));

    if (thisCrc != lastMeasurementCRC) {
        publishRelativeToBaseMQTT("status", buffer);
    }

    lastMeasurementCRC = thisCrc;
}

/**
 * Publish a message to mqtt
 */
void publishToMQTT(const char* topic, const char* payload) {
    if (!mqttClient.publish(topic, payload, true)) {
        Serial.println(F("Failed to publish"));
    }
}

void publishRelativeToBaseMQTT(const char* topic, const char* payload) {
    char buffer[65];
    const char* mqttBaseTopic = controllerConfig.get("mqttBaseTopic");
    strncpy(buffer, mqttBaseTopic, sizeof(buffer));
    strncat(buffer, "/", sizeof(buffer));
    strncat(buffer, topic, sizeof(buffer));
    publishToMQTT(buffer, payload);
}

/////////////////////////////////////////////////////////////////////////////////////

/**
 * Handle incomming MQTT requests
 */
void handleCmd(const char* topic, const char* p_payload) {
    const char* mqttClientID = controllerConfig.get("mqttClientID");
    uint8_t mqttSubscriberTopicStrLength = std::strlen(mqttClientID);

    auto topicPos = topic + mqttSubscriberTopicStrLength;
    // Serial.print(F("Handle command : "));
    // Serial.println(topicPos);

    char payloadBuffer[16];
    strncpy(payloadBuffer, p_payload, sizeof(payloadBuffer));
    if (std::strstr(topicPos, "/config") != nullptr) {
        bool on;
        OptParser::get(payloadBuffer, [&on](OptValue values) {

            if (std::strcmp(values.key(), "en") == 0) {
                controllerConfig.put("ringerOn", PV((int)values != 0));
                controllerConfigModified = true;
            }

        });

        Serial.println("Config");
    }

    if (strstr(topicPos, "/reset") != nullptr) {
        OptParser::get(payloadBuffer, [](OptValue v) {
            if (strcmp(v.key(), "1") == 0) {
                shouldRestart = millis();
            }
        });
    }

}

/**
 * Initialise MQTT and variables
 */
void setupMQTT() {

    mqttClient.setCallback([](char* p_topic, byte * p_payload, uint16_t p_length) {
        char mqttReceiveBuffer[64];
        //Serial.println(p_topic);

        if (p_length >= sizeof(mqttReceiveBuffer)) {
            return;
        }

        memcpy(mqttReceiveBuffer, p_payload, p_length);
        mqttReceiveBuffer[p_length] = 0;
        handleCmd(p_topic, mqttReceiveBuffer);
    });
}

///////////////////////////////////////////////////////////////////////////
//  WiFi
///////////////////////////////////////////////////////////////////////////


void serverOnlineCallback() {
}

/**
 * Setup statemachine that will handle reconnection to mqtt after WIFI drops
 */
void setupWIFIReconnectManager() {
    static char mqttLastWillTopic[64];
    const char* mqttClientID = controllerConfig.get("mqttClientID");
    snprintf(mqttLastWillTopic, sizeof(mqttLastWillTopic), "%s/%s", mqttClientID, MQTT_LASTWILL_TOPIC);

    // Statemachine to handle (re)connection to MQTT
    State* BOOTSEQUENCESTART = new State;
    State* DELAYEDMQTTCONNECTION = new StateTimed {1500};
    State* TESTMQTTCONNECTION = new State;
    State* CONNECTMQTT = new State;
    State* PUBLISHONLINE = new State;
    State* SUBSCRIBECOMMANDTOPIC = new State;
    State* WAITFORCOMMANDCAPTURE = new StateTimed { 3000 };

    BOOTSEQUENCESTART->setRunnable([TESTMQTTCONNECTION]() {
        return TESTMQTTCONNECTION;
    });
    DELAYEDMQTTCONNECTION->setRunnable([DELAYEDMQTTCONNECTION, TESTMQTTCONNECTION]() {
        hasMqttConfigured =
            controllerConfig.contains("mqttServer") &&
            std::strlen((const char*)controllerConfig.get("mqttServer")) > 0;

        if (!hasMqttConfigured) {
            return DELAYEDMQTTCONNECTION;
        }

        return TESTMQTTCONNECTION;
    });
    TESTMQTTCONNECTION->setRunnable([DELAYEDMQTTCONNECTION, TESTMQTTCONNECTION, CONNECTMQTT]() {
        if (mqttClient.connected())  {
            if (WiFi.status() != WL_CONNECTED) {
                mqttClient.disconnect();
            }

            return DELAYEDMQTTCONNECTION;
        }

        // For some reason the access point active, so we disable it explicitly
        // FOR ESP32 we will keep on this state untill WIFI is connected
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.mode(WIFI_STA);
        } else {
            return TESTMQTTCONNECTION;
        }

        return CONNECTMQTT;
    });
    CONNECTMQTT->setRunnable([PUBLISHONLINE, DELAYEDMQTTCONNECTION]() {
        mqttClient.setServer(
            controllerConfig.get("mqttServer"),
            (int16_t)controllerConfig.get("mqttPort")
        );

        if (mqttClient.connect(
                controllerConfig.get("mqttClientID"),
                controllerConfig.get("mqttUsername"),
                controllerConfig.get("mqttPassword"),
                mqttLastWillTopic,
                0,
                1,
                MQTT_LASTWILL_OFFLINE)
           ) {
            return PUBLISHONLINE;
        }

        return DELAYEDMQTTCONNECTION;
    });
    PUBLISHONLINE->setRunnable([SUBSCRIBECOMMANDTOPIC]() {
        publishToMQTT(
            mqttLastWillTopic,
            MQTT_LASTWILL_ONLINE);
        return SUBSCRIBECOMMANDTOPIC;
    });
    SUBSCRIBECOMMANDTOPIC->setRunnable([WAITFORCOMMANDCAPTURE, DELAYEDMQTTCONNECTION]() {
        char mqttSubscriberTopic[32];
        const char* mqttClientID = controllerConfig.get("mqttClientID");
        snprintf(mqttSubscriberTopic, sizeof(mqttSubscriberTopic), "%s/+", mqttClientID);

        if (mqttClient.subscribe(mqttSubscriberTopic, 0)) {
            return WAITFORCOMMANDCAPTURE;
        }

        mqttClient.disconnect();
        return DELAYEDMQTTCONNECTION;
    });
    WAITFORCOMMANDCAPTURE->setRunnable([TESTMQTTCONNECTION]() {
        return TESTMQTTCONNECTION;
    });
    bootSequence.reset(new StateMachine { BOOTSEQUENCESTART } );
    bootSequence->start();
}

///////////////////////////////////////////////////////////////////////////
//  Webserver/WIFIManager
///////////////////////////////////////////////////////////////////////////
void saveParamCallback() {
    Serial.println("[CALLBACK] saveParamCallback fired");

    if (std::strlen(wm_mqtt_server.getValue()) > 0) {
        controllerConfig.put("mqttServer", PV(wm_mqtt_server.getValue()));
        controllerConfig.put("mqttPort", PV(std::atoi(wm_mqtt_port.getValue())));
        controllerConfig.put("mqttUsername", PV(wm_mqtt_user.getValue()));
        controllerConfig.put("mqttPassword", PV(wm_mqtt_password.getValue()));
        controllerConfigModified = true;
        // Redirect from MQTT so on the next reconnect we pickup new values
        mqttClient.disconnect();
        // Send redirect back to param page
        wm.server->sendHeader(F("Location"), F("/param?"), true);
        wm.server->send(302, FPSTR(HTTP_HEAD_CT2), "");   // Empty content inhibits Content-length header so we have to close the socket ourselves.
        wm.server->client().stop();
    }
}

/**
 * Setup the wifimanager and configuration page
 */
void setupWifiManager() {
    char port[6];
    snprintf(port, sizeof(port), "%d", (int16_t)controllerConfig.get("mqttPort"));
    wm_mqtt_port.setValue(port, MQTT_PORT_LENGTH);
    wm_mqtt_password.setValue(controllerConfig.get("mqttPassword"), MQTT_PASSWORD_LENGTH);
    wm_mqtt_user.setValue(controllerConfig.get("mqttUsername"), MQTT_USERNAME_LENGTH);
    wm_mqtt_server.setValue(controllerConfig.get("mqttServer"), MQTT_SERVER_LENGTH);

    // Set extra setup page
    wm.setWebServerCallback(serverOnlineCallback);
    wm.addParameter(&wm_mqtt_server);
    wm.addParameter(&wm_mqtt_port);
    wm.addParameter(&wm_mqtt_user);
    wm.addParameter(&wm_mqtt_password);

    // set country
    wm.setClass("invert");
    wm.setCountry("US"); // setting wifi country seems to improve OSX soft ap connectivity, may help others as well

    // Set configuration portal
    wm.setShowStaticFields(false);
    wm.setConfigPortalBlocking(false); // Must be blocking or else AP stays active
    wm.setDebugOutput(false);
    wm.setSaveParamsCallback(saveParamCallback);
    wm.setHostname(controllerConfig.get("mqttBaseTopic"));
    std::vector<const char*> menu = {"wifi", "wifinoscan", "info", "param", "sep", "erase", "restart"};
    wm.setMenu(menu);

    wm.startWebPortal();
    wm.autoConnect(controllerConfig.get("mqttClientID"));
#if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    MDNS.begin(controllerConfig.get("mqttClientID"));
    MDNS.addService(0, "http", "tcp", 80);
#endif
}

///////////////////////////////////////////////////////////////////////////
//  SETUP and LOOP
///////////////////////////////////////////////////////////////////////////

void setupDefaults() {
    char chipHexBuffer[9];
    snprintf(chipHexBuffer, sizeof(chipHexBuffer), "%08X", ESP.getChipId());

    char mqttClientID[16];
    snprintf(mqttClientID, sizeof(mqttClientID), "DOORBELL%s", chipHexBuffer);

    char mqttBaseTopic[16] = "DOORBELL";

    char mqttLastWillTopic[64];
    snprintf(mqttLastWillTopic, sizeof(mqttLastWillTopic), "%s/%s", mqttBaseTopic, MQTT_LASTWILL_TOPIC);

    controllerConfigModified |= controllerConfig.putNotContains("mqttClientID", PV(mqttClientID));
    controllerConfigModified |= controllerConfig.putNotContains("mqttBaseTopic", PV(mqttBaseTopic));
    controllerConfigModified |= controllerConfig.putNotContains("mqttLastWillTopic", PV(mqttLastWillTopic));

    controllerConfigModified |= controllerConfig.putNotContains("mqttServer", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttUsername", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttPassword", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttPort", PV(1883));
    controllerConfigModified |= controllerConfig.putNotContains("mqttBaseTopic", PV(mqttBaseTopic));
    controllerConfigModified |= controllerConfig.putNotContains("ringerOn", PV(true));
    controllerConfigModified |= controllerConfig.putNotContains("maxRingTime", PV(5000));
}

void setup() {
    pinMode(RINGER_PIN, OUTPUT);
    digitalWrite(RINGER_PIN, INVERT_OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, 0);

    // Enable serial port
    Serial.begin(115200);
    delay(050);
    // load configurations
    loadConfig(CONFIG_FILENAME, controllerConfig);
    setupDefaults();

    setupMQTT();
    setupWifiManager();
    setupWIFIReconnectManager();
    digitalKnob.init();

    Serial.println(F("End Setup"));
    effectPeriodStartMillis = millis();

}

#define NUMBER_OF_SLOTS 10
void loop() {
    const uint32_t currentMillis = millis();

    if (currentMillis - effectPeriodStartMillis >= EFFECT_PERIOD_CALLBACK) {
        effectPeriodStartMillis += EFFECT_PERIOD_CALLBACK;
        counter50TimesSec++;

        // DigitalKnob (the button) must be handled at 50 times/sec to correct handle presses and double presses
        digitalKnob.handle();

        //////////////////////////
        // Record time when we started the bell
        if (digitalKnob.isEdgeUp()) {
            bellStartTime = currentMillis;
        }

        // Always show the digital led
        digitalWrite(LED_PIN, digitalKnob.current());

        // Ringer the bell when we have it enabled and when it´s within the allowed timeframe
        if (controllerConfig.get("ringerOn") &&
            (currentMillis - bellStartTime < (uint32_t)controllerConfig.get("maxRingTime").asLong())) {
            digitalWrite(RINGER_PIN, digitalKnob.current() ^ INVERT_OUTPUT);
        } else {
            digitalWrite(RINGER_PIN, INVERT_OUTPUT);
        }

        if (digitalKnob.isEdgeUp() || digitalKnob.isEdgeDown()) {
            publishStatusToMqtt();
        }

        //////////////////////////

        // Maintenance stuff
        uint8_t slot50 = 0;

        if (counter50TimesSec % NUMBER_OF_SLOTS == slot50++) {
            bootSequence->handle();
        } else if (counter50TimesSec % NUMBER_OF_SLOTS == slot50++) {
            mqttClient.loop();
        } else if (counter50TimesSec % NUMBER_OF_SLOTS == slot50++) {
            if (controllerConfigModified) {
                controllerConfigModified = false;
                publishStatusToMqtt();
                saveConfig(CONFIG_FILENAME, controllerConfig);
            }
        } else if (counter50TimesSec % NUMBER_OF_SLOTS == slot50++) {
            wm.process();
        } else if (shouldRestart != 0 && (currentMillis - shouldRestart >= 5000)) {
            shouldRestart = 0;
            ESP.restart();
        }
    }
}

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
#include <FS.h>   // Include the SPIFFS library

#include <propertyutils.h>
#include <optparser.h>
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
char* mqttLastWillTopic;
char* mqttClientID;
char* mqttSubscriberTopic;
uint8_t mqttSubscriberTopicStrLength;
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
//  Spiffs
///////////////////////////////////////////////////////////////////////////


bool loadConfigSpiffs(const char* filename, Properties& properties) {
    bool ret = false;

    if (SPIFFS.begin()) {
        Serial.println("mounted file system");

        if (SPIFFS.exists(filename)) {
            //file exists, reading and loading
            File configFile = SPIFFS.open(filename, "r");

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

        // SPIFFS.end();
    } else {
        Serial.print(F("Failed to begin SPIFFS"));
    }

    return ret;
}


/**
 * Store custom oarameter configuration in SPIFFS
 */
bool saveConfigSPIFFS(const char* filename, Properties& properties) {
    bool ret = false;

    if (SPIFFS.begin()) {
        SPIFFS.remove(filename);
        File configFile = SPIFFS.open(filename, "w");

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
        //    SPIFFS.end();
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
void publishToMQTT(const char* topic, const char* payload);
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
        publishToMQTT("status", buffer);
    }

    lastMeasurementCRC = thisCrc;
}

/**
 * Publish a message to mqtt
 */
void publishToMQTT(const char* topic, const char* payload) {
    char buffer[65];
    const char* mqttBaseTopic = controllerConfig.get("mqttBaseTopic");
    snprintf(buffer, sizeof(buffer), "%s/%s", mqttBaseTopic, topic);

    if (mqttClient.publish(buffer, payload, true)) {
    } else {
        Serial.println(F("Failed to publish"));
    }
}

/////////////////////////////////////////////////////////////////////////////////////

/**
 * Handle incomming MQTT requests
 */
void handleCmd(const char* topic, const char* p_payload) {
    auto topicPos = topic + mqttSubscriberTopicStrLength;
    //Serial.print(F("Handle command : "));
    //Serial.println(topicPos);

    // Look for a temperature setPoint topic
    if (std::strstr(topicPos, "config") != nullptr) {
        bool on;
        OptParser::get(p_payload, [&on](OptValue values) {

            if (std::strcmp(values.key(), "en") == 0) {
                controllerConfig.put("ringerOn", PV((int)values != 0));
                controllerConfigModified = true;
            }

        });

        Serial.println("Config");
    }

    if (strstr(topicPos, "reset") != nullptr) {
        OptParser::get(p_payload, [](OptValue v) {
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

    const char* mqttBaseTopic = controllerConfig.get("mqttBaseTopic");
    mqttClientID = makeCString("%08X", ESP.getChipId());
    mqttLastWillTopic = makeCString("%s/%s", mqttBaseTopic, MQTT_LASTWILL_TOPIC);
    mqttSubscriberTopic = makeCString("%s/+", mqttBaseTopic);
    mqttSubscriberTopicStrLength = std::strlen(mqttSubscriberTopic) - 1;
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
    // Statemachine to handle (re)connection to MQTT
    State* BOOTSEQUENCESTART;
    State* DELAYEDMQTTCONNECTION;
    State* TESTMQTTCONNECTION;
    State* CONNECTMQTT;
    State* PUBLISHONLINE;
    State* SUBSCRIBECOMMANDTOPIC;
    State* WAITFORCOMMANDCAPTURE;

    BOOTSEQUENCESTART = new State([]() {
        return 2;
    });
    DELAYEDMQTTCONNECTION = new StateTimed(1500, []() {
        hasMqttConfigured =
            controllerConfig.contains("mqttServer") &&
            std::strlen((const char*)controllerConfig.get("mqttServer")) > 0;

        if (!hasMqttConfigured) {
            return 1;
        }

        return 2;
    });
    TESTMQTTCONNECTION = new State([]() {
        if (mqttClient.connected())  {
            if (WiFi.status() != WL_CONNECTED) {
                mqttClient.disconnect();
            }

            return 1;
        }

        WiFi.mode(WIFI_STA); // HACK for some reason AP stayed active, even though it should not
        return 3;
    });
    CONNECTMQTT = new State([]() {
        mqttClient.setServer(
            controllerConfig.get("mqttServer"),
            (int16_t)controllerConfig.get("mqttPort")
        );

        if (mqttClient.connect(
                mqttClientID,
                controllerConfig.get("mqttUsername"),
                controllerConfig.get("mqttPassword"),
                mqttLastWillTopic,
                0,
                1,
                MQTT_LASTWILL_OFFLINE)
           ) {

            return 4;
        }

        return 1;
    });
    PUBLISHONLINE = new State([]() {
        publishToMQTT(
            MQTT_LASTWILL_TOPIC,
            MQTT_LASTWILL_ONLINE);
        return 5;
    });
    SUBSCRIBECOMMANDTOPIC = new State([]() {
        if (mqttClient.subscribe(mqttSubscriberTopic, 0)) {

            return 6;
        }

        mqttClient.disconnect();
        return 1;
    });
    WAITFORCOMMANDCAPTURE = new StateTimed(3000, []() {
            publishStatusToMqtt();
        return 2;
    });
    bootSequence.reset(new StateMachine({
        BOOTSEQUENCESTART, // 0
        DELAYEDMQTTCONNECTION,// 1
        TESTMQTTCONNECTION, // 2
        CONNECTMQTT, // 3
        PUBLISHONLINE, // 4
        SUBSCRIBECOMMANDTOPIC, // 5
        WAITFORCOMMANDCAPTURE // 6
    }));
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
    wm.autoConnect(controllerConfig.get("mqttBaseTopic"));
}

///////////////////////////////////////////////////////////////////////////
//  SETUP and LOOP
///////////////////////////////////////////////////////////////////////////

void setupDefaults() {
    controllerConfigModified |= controllerConfig.putNotContains("mqttServer", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttUsername", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttPassword", PV(""));
    controllerConfigModified |= controllerConfig.putNotContains("mqttPort", PV(1883));
    controllerConfigModified |= controllerConfig.putNotContains("mqttBaseTopic", PV("DOORBELL"));
    controllerConfigModified |= controllerConfig.putNotContains("ringerOn", PV(true));
    controllerConfigModified |= controllerConfig.putNotContains("maxRingTime", PV(5000));
}

void setup() {
    pinMode(RINGER_PIN,OUTPUT);
    digitalWrite(RINGER_PIN, INVERT_OUTPUT);
    pinMode(LED_PIN,OUTPUT);
    digitalWrite(LED_PIN, 0);

    // Enable serial port
    Serial.begin(115200);
    delay(050);
    // load configurations
    loadConfigSpiffs(CONFIG_FILENAME, controllerConfig);
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
        digitalWrite(LED_PIN, digitalKnob.current() ^ INVERT_OUTPUT ^ INVERT_INPUT);
        // Ringer the bell when we have it enabled and when it´s within the allowed timeframe
        if (controllerConfig.get("ringerOn") && 
            (currentMillis - bellStartTime < (int16_t)controllerConfig.get("maxRingTime"))) {
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
                saveConfigSPIFFS(CONFIG_FILENAME, controllerConfig);
            }
        } else if (counter50TimesSec % NUMBER_OF_SLOTS == slot50++) {
             wm.process();
        } else if (shouldRestart != 0 && (currentMillis - shouldRestart >= 5000)) {
            shouldRestart = 0;
            ESP.restart();
        }
    }
}

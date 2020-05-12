// How often we are updating the mqtt state in ms

#define MQTT_LASTWILL                           "lastwill"
#define MQTT_STATUS                           "status"
#define MQTT_LASTWILL_TOPIC                    "lastwill"
#define MQTT_LASTWILL_ONLINE                   "online"
#define MQTT_LASTWILL_OFFLINE                  "offline"

constexpr uint8_t BUTTON_PIN = 12;  // D6
constexpr uint8_t RINGER_PIN = 14;  // D5
constexpr uint8_t LED_PIN = 2;
constexpr bool INVERT_OUTPUT = false;
constexpr bool INVERT_INPUT = true;

#define CONFIG_FILENAME "doorbell.conf"

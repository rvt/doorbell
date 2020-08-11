// How often we are updating the mqtt state in ms

constexpr char MQTT_STATUS[] =                           "status";
constexpr char  MQTT_LASTWILL_TOPIC[] =                    "lastwill";
constexpr char  MQTT_LASTWILL_ONLINE[] =                   "online";
constexpr char  MQTT_LASTWILL_OFFLINE[] =                  "offline";

constexpr uint8_t BUTTON_PIN = 12;  // D6
constexpr uint8_t RINGER_PIN = 14;  // D5
constexpr uint8_t LED_PIN = 2;
constexpr bool INVERT_OUTPUT = false;
constexpr bool INVERT_INPUT = true;

constexpr char   CONFIG_FILENAME[] = "doorbell.conf";

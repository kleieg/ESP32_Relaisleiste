// set hostname used for MQTT tag and WiFi
#define HOSTNAME "ESPRelais"
#define VERSION "v 1.0.2"

#define BASELINE_INTERVAL 3600000
#define MQTT_INTERVAL 120000
#define RECONNECT_INTERVAL 5000
#define PUBLISH_DELAY 5000
#define LED_BLINK_INTERVAL 500
#define RELAY_RESET_INTERVAL 5000

//-------------------------------------------------------------------
#if defined(ESP8266)
    #define GPIO_LED 2 // D4

    // Set number of outputs
    #define NUM_OUTPUTS 4

    // Assign each GPIO to an output
    // D1, D2, D3, D5
    int outputGPIOs[NUM_OUTPUTS] = {5, 4, 0, 14};

    // Assign relay details
    String relayReset[NUM_OUTPUTS] = {"N", "N", "N", "N"};
    int relayResetStatus[NUM_OUTPUTS] = {0, 0, 0, 0};
    int relayResetTimer[NUM_OUTPUTS] = {0, 0, 0, 0};
#else
    #define GPIO_LED 1

    // Set number of outputs
    #define NUM_OUTPUTS 7 // nur 7 ansprechbare Relais !!!

    // Assign each GPIO to an output
    int outputGPIOs[NUM_OUTPUTS] = {16, 17, 18, 19, 21, 22, 23};

    // Assign relay details
    String relayReset[NUM_OUTPUTS] = {"N", "N", "N", "N", "N", "N", "N"};
    int relayResetStatus[NUM_OUTPUTS] = {0, 0, 0, 0, 0, 0, 0};
    int relayResetTimer[NUM_OUTPUTS] = {0, 0, 0, 0, 0, 0, 0};
#endif
//-------------------------------------------------------------------

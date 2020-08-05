[![Build Status](https://www.travis-ci.org/rvt/doorbell.svg?branch=master)](https://www.travis-ci.org/rvt/doorbell)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
# Doorbell

It´s code for a remote doorbell. Nothing fancy.. It just allows me to send a push notification to Telegram and Home app.

If you wish to use this code, feel free to do so.
It was designed around a Wemos D1:

# Features

* Allows to ring your current buzzer using a relay
* Allows to turn off/on your buzzer while we keep sending messages to telegram/homeapp
* Allows you to send a request over MQTT
* Uses a filter to prevent false positives

# Hardware configuration

```
BUTTON_PIN = 12;        // D6 - Pin of the button
RINGER_PIN = 14;        // D5 - Pin of the relay for your ringer
LED_PIN = 2;            // Standard pin of the wemos led
INVERT_OUTPUT = false;  // Invertrs the input, if you need that...
INVERT_INPUT = true;    // Inverts the output, if you need that...
```

# Software configuration

After the Wemos is flashed if will create a Accesspoint called *DOORBELL*
Access it with your computer and configure your WIFI and MQTT server, then reboot
and I prefere even that you will remove power and add it again (ESP oddities).

# MQTT

There are just a few topics the DOORBELL listens on and send messages to.

### Topic: DOORBELL/config
Send `en=1` to enable the buzzer.
Send `en=0` to disable the buzzer.

### Topic: DOORBELL/status
Receive `en=1` when the buzzer is enabled.
Receive `en=0` when the buzzer is enabled.
Receive `ri=1` when the button is pressed.
Receive `ri=0` when the button is not presses.

Note: When `en=0` the buzzer will not be enabled, but we do send the `ri=1` message.

# Compilation Upload

````
platformio run --target upload
````

# Hardware

* Use a velleman relays on pin D5
* I use a simple push button wuth pull-up of 1KOhm and two diodes for over voltage prevention on D6

# Homebridge configuration

You need to use MQTT thing.
If you want to use the motionSensor, don´t forget the enable notifications so you can get an actual alert.

````
{
    "accessory": "mqttthing",
    "type": "motionSensor",
    "name": "Front Doorbell",
    "caption": "Front doorbell",
    "url": "http://localhost:1883",
    "username": "<USERNAME>",
    "password": "<PASSWORD>",
    "topics": {
        "getMotionDetected": {
            "topic": "DOORBELL/status",
            "apply": "return message.includes('ri=1')"
        },
        "getStatusActive": {
            "topic": "DOORBELL/status",
            "apply": "return true"
        }
    },
    "turnOffAfterms": "6000"
},
{
    "accessory": "mqttthing",
    "type": "outlet",
    "name": "Doorbell Buzzer",
    "url": "http://localhost:1883",
    "username": "<USERNAME>",
    "password": "<PASSWORD>",
    "mqttPubOptions": {
        "retain": true
    },
    "caption": "Doorbell Buzzer",
    "topics": {
        "getOnline": {
            "topic": "DOORBELL/lastwill",
            "apply": "return message.includes('online')"
        },
        "getOn": {
            "topic": "DOORBELL/status",
            "apply": "return message.includes('en=1')"
        },
        "setOn": {
            "topic": "DOORBELL/config",
            "apply": "return message?'en=1':'en=0'"
        }
    }
}
```
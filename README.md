# ledash
IoT LED Dashboard

This little tool turns your ESP8266 (took a Wemos D1 mini) and some WS2812b into a nice IoT-Dashboard with immediate overview.
Put the LEDs in a small enclosure (Design WIP...), have seperators for each LED and put a sheet of paper on that. Mark which light shows what element and there you go.
Once configured to WiFi and MQTT (Homie configuration!) send control messages to the board to the configured topic /homepath/deviceid/control/status/set.
Use "n=M" text format to set state number n to state M (alphanumerical).
The associated LED smoothly changes its color to the one of the new state. Done!

Build instructions: 
 - connect data pin of WS2812 to LED_PIN and TEMT6000 (3.3v) to pin LIGHT_SENSOR
 - Configure the predefined states colors in setup()
 - Set overall brightness and heat/cool-down values so newly set values are brighter.
 - Map states to LEDs (code only so far)

unimplemented at the moment:
 - change brightness/cool-down/cool-down-time via MQTT
 - change mapping of LEDs to states via MQTT
 - store configuration in filesystem

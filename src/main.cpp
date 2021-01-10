/*
 * IoT Dashboard
 * Author: Lars Friedrichs
 * License: GPL2
 * 
 * This little tool turns your ESP8266 (took a Wemos D1 mini) and some WS2812b into
 * a nice IoT-Dashboard with immediate overview.
 * Put the LEDs in a small enclosure, have seperators for each LED and put a sheet of
 * paper on that. Mark which light shows what element and there you go.
 * One configured to WiFi and MQTT (Homie configuration!) send control messages to the board
 * to the configured topic /homepath/deviceid/control/status/set.
 * Use "n=M" text format to set state number n to state M (alphanumerical).
 * The associated LED smoothly changes its color to the one of the new state. Done!
 * 
 * Build instructions: connect data pin of WS2812 to LED_PIN and TEMT6000 (3.3v) to pin LIGHT_SENSOR
 * Configure the predefined states colors in setup()
 * Set overall brightness and heat/cool-down values so newly set values are brighter.
 * Map states to LEDs (code only so far)
 * 
 * unimplemented at the moment:
 *  - change brightness/cool-down/cool-down-time via MQTT
 *  - change mapping of LEDs to states via MQTT
 *  - store configuration in filesystem
 */

#include <Arduino.h>
#include "Homie.h"
#include "FastLED.h"
#include "RunningAverage.h"

#define LIGHT_SENSOR A0
#define LED_PIN     D2
#define COLOR_ORDER GRB
#define CHIPSET     WS2812B
#define NUM_LEDS_MAX      50   // this needs to be lower than 254 because of byte values used... 
#define FRAMES_PER_SECOND 50
#define FRAMES_PER_FADE ((FRAMES_PER_SECOND * 1.4) / 2)
#define NO_FADE (FRAMES_PER_FADE + 2)

#define BRIGHTNESS_HIGH  128      // preset overall brightness (aka max. brightness)
#define BRIGHTNESS_LOW  12        // preset overall brightness (aka max. brightness)
#define BRIGHTNESS_COLD 128       // relative brightness to global brightness value (128 = half as bright)
#define COOL_DOWN_TIME 30         // seconds after change "cold" brightness is reached
#define SENSOR_CURVE 0.35         // exponent for relative (0..1) light sensor readings 

const String POSSIBLE_STATES = "0123456789abcdefghijklmnopqrstuvwxyz-_:.?!$%/<>ABCDEFGHIJKLMNOPQRSTUVWXYZ ";

uint8_t state[NUM_LEDS_MAX];      // stores the actual state
uint8_t stateNext[NUM_LEDS_MAX];  // stores the next state after fading out
int8_t stateFader[NUM_LEDS_MAX];  // stores the value, how far fading has progressed
CRGB leds[NUM_LEDS_MAX+1];        // stores the led's colors
uint16_t led_count = NUM_LEDS_MAX;  // this should be customizable later
uint8_t brightness_low  = BRIGHTNESS_LOW;    // this should be customizable later
uint8_t brightness_high = BRIGHTNESS_HIGH;    // this should be customizable later
uint8_t brightness_cold = BRIGHTNESS_COLD;  // this should be customizable later
uint8_t cool_down_time = COOL_DOWN_TIME;    // this should be customizable later
float sensor_curve_calibration = 0.45;
CHSV stateColor[256];             // stores the color to each state - we are not using all the states, I know...

uint8_t mapping[NUM_LEDS_MAX+1]; // this way we can map all inputs at different places later
uint8_t heat[NUM_LEDS_MAX];    // fresh changes should be brighter

HomieNode controlNode("control","Control LEDs","controller");  // this is to control the dashboard
HomieNode configNode("config","Configuration","config");

RunningAverage avg(50);

// sends the active status via MQTT
void sendStatus() {
  String s = "";
  // yes, there might be better ways to do this...
  for (int i=0; i<NUM_LEDS_MAX; i++) {
    s+=POSSIBLE_STATES[state[i]];
  }
  controlNode.setProperty("status").send(s);
}

// simply returns true if the given string is numeric (either integer or decimal)
boolean isNumeric(String str) {
  unsigned int stringLength = str.length();
  if (stringLength == 0) return false;

  boolean seenDecimal = false;
  for(unsigned int i = 0; i < stringLength; ++i) {
    if (isDigit(str.charAt(i))) continue;
    if (str.charAt(i) == '.') {
      if (seenDecimal) return false;
      seenDecimal = true;
      continue;
    }
    return false;
  }
  return true;
}

// change the state of a given position to new_state
// but do nothing if it is already in that state
void changeState(uint16_t position, uint8_t new_state) {
  if (position < led_count) {
    if (stateFader[position]== NO_FADE) {
      // no fading going on, so let's set the next state and start the fade
      if (state[position] != new_state) {
        stateNext[position] = new_state;
        stateFader[position] = FRAMES_PER_FADE;
      }
    } else {
      // we are already fading, so let us be careful not to destroy the eye candy smooth fading :-)
      if (stateFader[position]>0) {
        // we are still fading out, so we can just change the next state
        stateNext[position] = new_state;
      } else {
        // we are already fading in, so we set the next state and inverse the fading
        state[position] = stateNext[position];
        stateNext[position] = new_state;
        stateFader[position] = -stateFader[position];
        sendStatus(); // send status because we changed state[]
      }
    }
  }
}

// this handler takes the new values from MQTT and sets them
// sending n=M to the payload will change state n (numeric) to state M (alphanumeric, see POSSIBLE_STATES)
// every change and a payload of "?" will result in a status update via MQTT
bool statusHandler(const HomieRange& range, const String& value) {
//  Serial.println("  controlNode statusHandler called with value:" + value);
  if (value.equals("?")) {
    // simple status message requested, no change
    sendStatus();
    return true;
  }

  int p = value.indexOf("=");
//  Serial.print("  p:"); Serial.println(p);

  // we start at 1
  if (p<1) return false;
  const String v1 = value.substring(0, p);
  const String v2 = value.substring(p+1);
//  Serial.println("  v1:" + v1);
//  Serial.println("  v2:" + v2);
  if (!isNumeric(v1)) return false;
  int v = v1.toInt();
  if (v2.length()!=1) return false;
  if (!(v<led_count)) return false;
  if (POSSIBLE_STATES.indexOf(v2)==-1) return false;
  changeState(v, POSSIBLE_STATES.indexOf(v2));
  return true;
}

// calculate the colors and especially their values (in HSV mode)
// it respects and calculates the fading from one to the next state
// full heat is applied after zero is crossed
void doFading() {
  for (int j=0; j<NUM_LEDS_MAX; j++) {
    if (stateFader[j]!= NO_FADE) {
      // ok, we are fading
      if (stateFader[j]>0) {
        // we are still fading out, keep going
        CHSV c = stateColor[state[j]];
        c.val = map(stateFader[j],0,FRAMES_PER_FADE,0,c.val);
        c.val = map(heat[j],0,255,0,c.val);
        leds[mapping[j]] = c;
        stateFader[j]--;
      } else {
        // we have crossed zero and are fading in, so heat to the max
        heat[j] = 255;
        CHSV c = stateColor[stateNext[j]];
        c.val = map(stateFader[j],0,-FRAMES_PER_FADE,0,c.val);
        c.val = map(heat[j],0,255,0,c.val);
        leds[mapping[j]] = c;
        stateFader[j]--;
      }
      if (stateFader[j]< -FRAMES_PER_FADE) {
        // ok, fading is done, target color reached
        state[j] = stateNext[j];
        CHSV c = stateColor[state[j]];
        c.val = map(heat[j],0,255,0,c.val);
        leds[mapping[j]] = c;
        stateFader[j] = NO_FADE;
        sendStatus(); // send status because we changed state[]
      }
    } else {
      // no fading, so take state's color and cool applied to active heat
      CHSV c = stateColor[state[j]];
      c.val = map(heat[j],0,255,0,c.val);
      leds[mapping[j]] = c;
    }
  }
}

// cool down brightness after changes
void doCooling() {
  for (int j=0; j<NUM_LEDS_MAX; j++) {
    if (heat[j]> brightness_cold) {
      // simple function to reduce heat to the value of "cold" (brightness calculates linear but is sensed logarithmically)
      heat[j]--;
    }
  }
}

// setup - see the debug output for documentation
void setup() {
  Serial.begin(115200);
  Serial.println(F("Starting IoT-Dashboard..."));
  stateColor[0] = rgb2hsv_approximate(CRGB::Black);
  stateColor[1] = rgb2hsv_approximate(CRGB::Black);
  stateColor[2] = rgb2hsv_approximate(CRGB::Red);
  stateColor[3] = rgb2hsv_approximate(CRGB::Yellow);
  stateColor[4] = rgb2hsv_approximate(CRGB::Green);
  stateColor[5] = rgb2hsv_approximate(CRGB::Blue);
  stateColor[6] = rgb2hsv_approximate(CRGB::Violet);

  delay(500);
  Serial.print(F("...initializing FastLed ..."));
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS_MAX+1).setCorrection( UncorrectedColor );
  FastLED.setBrightness(brightness_high);
  for (int i=-1; i<=NUM_LEDS_MAX; i++) {
    for (int j=0; j<NUM_LEDS_MAX; j++) {
      if (i==j) {
        leds[j] = CRGB::White;
      } else {
        leds[j] = CRGB::Black;
      }
      state[j] = 0;
      stateNext[j] = 0;
      stateFader[j] = NO_FADE;
    }
    FastLED.show();
    delay(50);
  }
  for (int i = 0; i<=NUM_LEDS_MAX; i++) {
    mapping[i] = i;
  }
  mapping[NUM_LEDS_MAX] = NUM_LEDS_MAX;
  FastLED.setDither( 1 );     // activate temporal dithering
  Serial.println(F("done."));

  delay(100);
  Serial.print(F("...initializing Homie ..."));
  Homie_setFirmware("IoT-Dashboard", "0.1"); // The underscore is not a typo! See Magic bytes
  controlNode.advertise("status").settable(statusHandler); // save the configuration 
  Homie.setup();
  Serial.println(F("done."));

  pinMode(LIGHT_SENSOR,  INPUT); 
  avg.clear();
  avg.addValue(1);
}

// reads the light sensor and calculates the new brightness
void getLightSensor() {
  float reading = analogRead(LIGHT_SENSOR);                   // get light level
  float square_ratio = reading / 1023.0;                      // normalize sensor value
  square_ratio = pow(square_ratio, sensor_curve_calibration); // exponential function to correct light detecting curve
  avg.addValue(square_ratio);                                 // insert into running average
  FastLED.setBrightness(map(avg.getAverage()*255,0,255,brightness_low,brightness_high));
}

// let the show begin
void loop() {
  Homie.loop();  // do the "Homie" thing
  EVERY_N_MILLIS((cool_down_time * 1000) / (255 - brightness_cold)) { 
    doCooling();
  }
  EVERY_N_MILLIS(100) {
    getLightSensor();
  }
  EVERY_N_MILLIS(1000 / FRAMES_PER_SECOND) {
    doFading();
  }
  FastLED.show(); // put this outside the per frame function to make temporal dithering smooth
}
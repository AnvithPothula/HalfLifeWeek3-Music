#include <Arduino.h>
#include "USB.h"
#include "USBMIDI.h"
#include <Adafruit_NeoPixel.h>

USBMIDI MIDI;

const int SLIDER_PIN = 1;
const int LED_PIN    = 2;
const int NUM_LEDS   = 3;

const int ENC1_A_PIN  = 3;
const int ENC1_B_PIN  = 4;
const int ENC1_SW_PIN = 5;

const int ENC2_A_PIN  = 6;
const int ENC2_B_PIN  = 43; // TX
const int ENC2_SW_PIN = 44; // RX

const int TOUCH_PINS[7] = {13, 12, 11, 10, 9, 8, 7}; // TOUCH1-7, GP13-7
const uint8_t TOUCH_BASE_NOTES[7] = {60, 62, 64, 65, 67, 69, 71}; // C4-B4

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800); // neopixels

uint32_t touchBaselines[7] = {0};
int touchPlaying[7] = {-1, -1, -1, -1, -1, -1, -1}; // -1 = not playing

int octaveOffset = 0; // clamped b/w -4 and +4
int modCCValue   = 0; // clamped b/w 0 and 127
int lastSliderVal = -1;

int lastEnc1A = HIGH;
bool enc1SwWasPressed = false;
unsigned long lastEnc1SwTime = 0;

int lastEnc2A = HIGH;
bool enc2SwWasPressed = false;
unsigned long lastEnc2SwTime = 0;

// update neopixel colors
void updateLEDs() {
  bool anyKeyPressed = false;
  for (int i = 0; i < 7; i++) {
    if (touchPlaying[i] != -1) {
      anyKeyPressed = true;
      break;
    }
  }

  uint32_t color = 0;
  if (anyKeyPressed) {
    color = pixels.Color(255, 255, 255); // flash white when a key pressed
  } else {
    switch (octaveOffset) {
      case -4: color = pixels.Color(0, 0, 128);   break; // deep blue
      case -3: color = pixels.Color(0, 0, 255);   break; // blue
      case -2: color = pixels.Color(0, 128, 255); break; // sky blue
      case -1: color = pixels.Color(0, 255, 255); break; // cyan
      case  0: color = pixels.Color(0, 255, 0);   break; // green (I made this the default)
      case  1: color = pixels.Color(180, 255, 0); break; // lime
      case  2: color = pixels.Color(255, 255, 0); break; // yellow
      case  3: color = pixels.Color(255, 100, 0); break; // orange
      case  4: color = pixels.Color(255, 0, 0);   break; // red
      default: color = pixels.Color(0, 255, 0);   break;
    }
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void setup() {
  // encoder 1
  pinMode(ENC1_A_PIN, INPUT_PULLUP);
  pinMode(ENC1_B_PIN, INPUT_PULLUP);
  pinMode(ENC1_SW_PIN, INPUT_PULLUP);
  lastEnc1A = digitalRead(ENC1_A_PIN);

  // encoder 2
  pinMode(ENC2_A_PIN, INPUT_PULLUP);
  pinMode(ENC2_B_PIN, INPUT_PULLUP);
  pinMode(ENC2_SW_PIN, INPUT_PULLUP);
  lastEnc2A = digitalRead(ENC2_A_PIN);

  // slider res (12-bit: 0-4095)
  analogReadResolution(12);

  // neopixels
  pixels.begin();
  pixels.setBrightness(75); // 0-255

  // calibrate capacitive touch pads
  delay(300);
  for (int i = 0; i < 7; i++) {
    uint64_t sum = 0;
    for (int sample = 0; sample < 16; sample++) {
      sum += touchRead(TOUCH_PINS[i]);
      delay(5);
    }
    touchBaselines[i] = sum / 16;
  }

  MIDI.begin();
  USB.begin();

  updateLEDs();
}

void loop() {
  unsigned long now = millis();

  // read encoder 1 (the octave shift one)
  int currentEnc1A = digitalRead(ENC1_A_PIN);
  if (currentEnc1A != lastEnc1A && currentEnc1A == LOW) {
    if (digitalRead(ENC1_B_PIN) != currentEnc1A) {
      octaveOffset = min(4, octaveOffset + 1);
    } else {
      octaveOffset = max(-4, octaveOffset - 1);
    }
    updateLEDs();
  }
  lastEnc1A = currentEnc1A;

  // clicking encoder 1 (resets octave to 0)
  bool enc1Pressed = (digitalRead(ENC1_SW_PIN) == LOW);
  if (enc1Pressed && !enc1SwWasPressed && (now - lastEnc1SwTime > 50)) {
    octaveOffset = 0;
    updateLEDs();
    enc1SwWasPressed = true;
    lastEnc1SwTime = now;
  } else if (!enc1Pressed && (now - lastEnc1SwTime > 50)) {
    enc1SwWasPressed = false;
  }

  // read encoder 2 (the mod wheel one)
  int currentEnc2A = digitalRead(ENC2_A_PIN);
  if (currentEnc2A != lastEnc2A && currentEnc2A == LOW) {
    if (digitalRead(ENC2_B_PIN) != currentEnc2A) {
      modCCValue = min(127, modCCValue + 4);
    } else {
      modCCValue = max(0, modCCValue - 4);
    }
    MIDI.controlChange(1, modCCValue, 1);
  }
  lastEnc2A = currentEnc2A;

  // clicking encoder 2 (resets modulation to 0)
  bool enc2Pressed = (digitalRead(ENC2_SW_PIN) == LOW);
  if (enc2Pressed && !enc2SwWasPressed && (now - lastEnc2SwTime > 50)) {
    modCCValue = 0;
    MIDI.controlChange(1, modCCValue, 1);
    enc2SwWasPressed = true;
    lastEnc2SwTime = now;
  } else if (!enc2Pressed && (now - lastEnc2SwTime > 50)) {
    enc2SwWasPressed = false;
  }

  // scan keys
  for (int i = 0; i < 7; i++) {
    uint32_t reading = touchRead(TOUCH_PINS[i]);
    bool isTouched = (reading > (touchBaselines[i] + (touchBaselines[i] / 4)));

    if (isTouched && touchPlaying[i] == -1) {
      int note = TOUCH_BASE_NOTES[i] + (octaveOffset * 12);
      note = max(0, min(127, note));
      MIDI.noteOn(note, 127, 1);
      touchPlaying[i] = note;
      updateLEDs();
    } else if (!isTouched && touchPlaying[i] != -1) {
      MIDI.noteOff(touchPlaying[i], 0, 1);
      touchPlaying[i] = -1;
      updateLEDs();
    }
  }

  // reads volume slider
  int rawSlider = analogRead(SLIDER_PIN); // 0-4095
  int sliderCC = rawSlider >> 5;
  if (abs(sliderCC - lastSliderVal) > 1) {
    MIDI.controlChange(7, sliderCC, 1);
    lastSliderVal = sliderCC;
  }

  delay(2);
}

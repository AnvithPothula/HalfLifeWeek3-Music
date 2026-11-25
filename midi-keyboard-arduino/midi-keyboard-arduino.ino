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

uint32_t touchBase[7] = {0};
int playing[7] = {-1, -1, -1, -1, -1, -1, -1}; // -1 = not playing

int octave = 0; // clamped b/w -4 and +4
int modVal   = 0; // clamped b/w 0 and 127
int lastSlider = -1;

int lastEnc1A = HIGH;
bool sw1Was = false;
unsigned long sw1Time = 0;

int lastEnc2A = HIGH;
bool sw2Was = false;
unsigned long sw2Time = 0;

// update neopixel colors
void updateLEDs() {
  bool anyKey = false;
  for (int i = 0; i < 7; i++) {
    if (playing[i] != -1) {
      anyKey = true;
      break;
    }
  }

  uint32_t color = 0;
  if (anyKey) {
    color = pixels.Color(255, 255, 255); // flash white when a key pressed
  } else {
    switch (octave) {
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
    touchBase[i] = sum / 16;
  }

  MIDI.begin();
  USB.begin();

  updateLEDs();
}

void loop() {
  unsigned long now = millis();

  // read encoder 1 (the octave shift one)
  int enc1A = digitalRead(ENC1_A_PIN);
  if (enc1A != lastEnc1A && enc1A == LOW) {
    if (digitalRead(ENC1_B_PIN) != enc1A) {
      octave = min(4, octave + 1);
    } else {
      octave = max(-4, octave - 1);
    }
    updateLEDs();
  }
  lastEnc1A = enc1A;

  // clicking encoder 1 (resets octave to 0)
  bool sw1 = (digitalRead(ENC1_SW_PIN) == LOW);
  if (sw1 && !sw1Was && (now - sw1Time > 50)) {
    octave = 0;
    updateLEDs();
    sw1Was = true;
    sw1Time = now;
  } else if (!sw1 && (now - sw1Time > 50)) {
    sw1Was = false;
  }

  // read encoder 2 (the mod wheel one)
  int enc2A = digitalRead(ENC2_A_PIN);
  if (enc2A != lastEnc2A && enc2A == LOW) {
    if (digitalRead(ENC2_B_PIN) != enc2A) {
      modVal = min(127, modVal + 4);
    } else {
      modVal = max(0, modVal - 4);
    }
    MIDI.controlChange(1, modVal, 1);
  }
  lastEnc2A = enc2A;

  // clicking encoder 2 (resets modulation to 0)
  bool sw2 = (digitalRead(ENC2_SW_PIN) == LOW);
  if (sw2 && !sw2Was && (now - sw2Time > 50)) {
    modVal = 0;
    MIDI.controlChange(1, modVal, 1);
    sw2Was = true;
    sw2Time = now;
  } else if (!sw2 && (now - sw2Time > 50)) {
    sw2Was = false;
  }

  // scan keys
  for (int i = 0; i < 7; i++) {
    uint32_t rd = touchRead(TOUCH_PINS[i]);
    bool touched = (rd > (touchBase[i] + (touchBase[i] / 4)));

    if (touched && playing[i] == -1) {
      int note = TOUCH_BASE_NOTES[i] + (octave * 12);
      note = max(0, min(127, note));
      MIDI.noteOn(note, 127, 1);
      playing[i] = note;
      updateLEDs();
    } else if (!touched && playing[i] != -1) {
      MIDI.noteOff(playing[i], 0, 1);
      playing[i] = -1;
      updateLEDs();
    }
  }

  // reads volume slider
  int raw = analogRead(SLIDER_PIN); // 0-4095
  int cc = raw >> 5;
  if (abs(cc - lastSlider) > 1) {
    MIDI.controlChange(7, cc, 1);
    lastSlider = cc;
  }

  delay(2);
}

/// @file    M5HeadBand.ino
/// @brief   M5 LED Headband with Larry's Pattern System + ESP-NOW Sync + Music Mode
/// @version 1.0.0
/// @date    2024-11-30
/// @author  John Cohn
///
/// @description
///   Combines the smooth parametric pattern transitions from larry_v3.ono
///   with the ESP-NOW synchronization and music reactivity from m5lights_v1.
///   Features dual-buffer compositing, alpha channel transitions, and full
///   beat detection (display only - patterns don't react to beats yet).
///
/// @changelog
/// v1.0.0 (2024-11-30) - Initial M5HeadBand Release
///   - Ported larry_v3.ono dual-buffer transition system
///   - Ported 4 parametric effects: Solid, Rainbow, Sine Wave, Wavy Flag
///   - Ported 3 alpha transitions: Fade, Wipe, Dither
///   - Kept m5lights_v1 ESP-NOW sync (Normal/Music/Leader modes)
///   - Kept m5lights_v1 beat detection and BPM display
///   - Kept m5lights_v1 sound-reactive brightness (with decay envelope)
///   - Converted from LPD8806 (7-bit) to WS2812B (8-bit) LEDs
///   - Converted from Timer1 interrupts to loop-based rendering
///   - Scaled from 20 LEDs to 200 LEDs
///   - Beat detection active but patterns don't respond to beats yet

#include <M5StickCPlus2.h>
#include <FastLED.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

FASTLED_USING_NAMESPACE

// Version info
#define VERSION "1.0.0"
#define BUILD 8  // Increment with each upload

// Hardware config
#define LED_PIN 32
#define NUM_LEDS 200
#define BRIGHTNESS 25
#define COLOR_ORDER GRB
#define CHIPSET WS2811
#define FRAMES_PER_SECOND 60

// Leader sync delay
#define LEADER_DELAY_MS 50

// Larry's Pattern System - Dual Buffer Architecture
// Principle: Two image buffers (back/front) with alpha channel for smooth transitions
byte imgData[2][NUM_LEDS * 3];  // Data for 2 complete LED strips worth of imagery
byte alphaMask[NUM_LEDS];        // Alpha channel for compositing images
byte backImgIdx = 0;             // Index of 'back' image (always 0 or 1)
byte fxIdx[3];                   // Effect # for back & front images + alpha
int  fxVars[3][50];              // Effect instance variables (state storage)
int  tCounter   = -1;            // Countdown to next transition
int  transitionTime;             // Duration (in frames) of current transition

// FastLED buffer (for display)
CRGB leds[NUM_LEDS];

// Pattern effect function declarations
void renderEffect00(byte idx);  // Solid color
void renderEffect01(byte idx);  // Rainbow
void renderEffect02(byte idx);  // Sine wave chase
void renderEffect03(byte idx);  // Wavy flag

// Alpha transition function declarations
void renderAlpha00(void);  // Simple fade
void renderAlpha01(void);  // Wipe
void renderAlpha02(void);  // Dither reveal

// Function arrays for pattern/transition selection
void (*renderEffect[])(byte) = {
  renderEffect00,
  renderEffect01,
  renderEffect02,
  renderEffect03
};

void (*renderAlpha[])(void) = {
  renderAlpha00,
  renderAlpha01,
  renderAlpha02
};

// Ultra-Simple Mode System from m5lights_v1
enum NodeMode {
  MODE_NORMAL,        // Standalone normal patterns, no sync
  MODE_MUSIC,         // Standalone music-reactive patterns, no sync
  MODE_NORMAL_LEADER, // Normal patterns + broadcast LED data
  MODE_MUSIC_LEADER   // Music patterns + broadcast LED data
};

// Simple LED data sync message
struct LEDSync {
  uint8_t startIndex;       // LED start position (0-199)
  uint8_t count;            // Number of LEDs in this packet (1-50)
  uint8_t sequenceNum;      // Packet sequence for ordering
  uint8_t brightness;       // Current brightness (for audio sync)
  uint8_t rgbData[149];     // RGB data (max 49 LEDs = 147 bytes)
};

// Global variables
NodeMode currentMode = MODE_NORMAL;
unsigned long lastModeSwitch = 0;
bool leaderDataActive = false;
unsigned long lastLeaderMessage = 0;
unsigned long lastCompleteFrame = 0;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Audio system variables (from m5lights_v1)
float soundMin = 1.0f;
float soundMax = 0.0f;
float musicLevel = 0.0f;
float audioLevel = 0.0f;
bool beatDetected = false;
bool prevAbove = false;
uint32_t beatTimes[50];
uint8_t beatCount = 0;
uint32_t beatIntervals[50];
uint8_t intervalCount = 0;
uint32_t lastBeatTime = 0;
uint32_t lastBpmMillis = 0;
bool audioDetected = true;
uint8_t musicBrightness = BRIGHTNESS;
unsigned long lastMusicDetectedTime = 0;
float currentBPM = 0.0f;

// Brightness decay envelope for smoother audio response
float brightnessEnvelope = BRIGHTNESS;
unsigned long lastBrightnessUpdate = 0;
#define BRIGHTNESS_DECAY_SECONDS 0.25f
#define BRIGHTNESS_THRESHOLD 0.35f
#define BRIGHTNESS_POWER_CURVE 2.0f

// Adaptive audio scaling
float noiseFloor = 0.01f;
float peakLevel = 0.1f;
float noiseFloorSmooth = 0.7f;
float peakLevelSmooth = 0.5f;

// ESP-NOW rejoin logic
unsigned long lastRejoinScan = 0;
bool rejoinMode = false;
uint8_t rejoinAttempts = 0;

// Audio configuration
static constexpr size_t MIC_BUF_LEN = 240;
static constexpr int MIC_SR = 44100;
static constexpr float SMOOTH = 0.995f;
static constexpr uint32_t BPM_WINDOW = 5000;

// Button handling
volatile bool buttonStateChanged = false;
volatile bool buttonCurrentState = false;
volatile unsigned long buttonLastInterrupt = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBroadcast = 0;

// Timing constants
#define LONG_PRESS_TIME_MS 1500
#define BROADCAST_INTERVAL_MS 50
#define LEADER_TIMEOUT_MS 8000
#define REJOIN_SCAN_INTERVAL_MS 15000
#define COMPLETE_FRAME_TIMEOUT_MS 5000

// Pattern names for display
const char* patternNames[] = {
  "Solid", "Rainbow", "SineWave", "Flag"
};

// ---------------------------------------------------------------------------
// Larry's Utility Functions - Ported from LPD8806 to WS2812B
// ---------------------------------------------------------------------------

// Gamma correction table - modified for 8-bit WS2812B (was 7-bit for LPD8806)
// This compensates for human eye's nonlinear perception of brightness
PROGMEM const uint8_t gammaTable[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 31, 32, 33, 33, 34, 35, 35,
   36, 37, 38, 38, 39, 40, 41, 41, 42, 43, 44, 45, 46, 46, 47, 48,
   49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
   65, 66, 67, 68, 69, 70, 72, 73, 74, 75, 76, 77, 79, 80, 81, 82,
   84, 85, 86, 87, 89, 90, 91, 93, 94, 95, 97, 98, 99,101,102,104,
  105,107,108,110,111,113,114,116,117,119,120,122,124,125,127,129,
  130,132,134,135,137,139,141,142,144,146,148,150,151,153,155,157,
  159,161,163,165,167,169,171,173,175,177,179,181,183,185,187,190,
  192,194,196,198,201,203,205,208,210,212,215,217,220,222,225,227,
  230,232,235,237,240,243,245,248,251,253,255
};

inline byte applyGamma(byte x) {
  return pgm_read_byte(&gammaTable[x]);
}

// HSV to RGB conversion
// Hue units: 1536 increments around color wheel (not degrees)
// Saturation: 0-255, Value: 0-255
long hsv2rgb(long h, byte s, byte v) {
  byte r, g, b, lo;
  int  s1;
  long v1;

  // Hue
  h %= 1536;
  if(h < 0) h += 1536;
  lo = h & 255;
  switch(h >> 8) {
    case 0 : r = 255     ; g =  lo     ; b =   0     ; break; // R to Y
    case 1 : r = 255 - lo; g = 255     ; b =   0     ; break; // Y to G
    case 2 : r =   0     ; g = 255     ; b =  lo     ; break; // G to C
    case 3 : r =   0     ; g = 255 - lo; b = 255     ; break; // C to B
    case 4 : r =  lo     ; g =   0     ; b = 255     ; break; // B to M
    default: r = 255     ; g =   0     ; b = 255 - lo; break; // M to R
  }

  // Saturation
  s1 = s + 1;
  r = 255 - (((255 - r) * s1) >> 8);
  g = 255 - (((255 - g) * s1) >> 8);
  b = 255 - (((255 - b) * s1) >> 8);

  // Value (brightness) and 24-bit color concat
  v1 = v + 1;
  return (((r * v1) & 0xff00) << 8) |
          ((g * v1) & 0xff00)       |
         ( (b * v1)           >> 8);
}

// Sine table for wave effects (181 elements: 0 to 180 inclusive)
// Units: 1/2 degree (720 units around full circle)
PROGMEM const char sineTable[181] = {
    0,  1,  2,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 15, 16, 17,
   18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 32, 33, 34,
   35, 36, 37, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
   52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67,
   67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 77, 78, 79, 80, 81,
   82, 83, 83, 84, 85, 86, 87, 88, 88, 89, 90, 91, 92, 92, 93, 94,
   95, 95, 96, 97, 97, 98, 99,100,100,101,102,102,103,104,104,105,
  105,106,107,107,108,108,109,110,110,111,111,112,112,113,113,114,
  114,115,115,116,116,117,117,117,118,118,119,119,120,120,120,121,
  121,121,122,122,122,123,123,123,123,124,124,124,124,125,125,125,
  125,125,126,126,126,126,126,126,126,127,127,127,127,127,127,127,
  127,127,127,127,127
};

char fixSin(int angle) {
  angle %= 720;
  if(angle < 0) angle += 720;
  return (angle <= 360) ?
     pgm_read_byte(&sineTable[(angle <= 180) ?
       angle : (360 - angle)]) :
    -pgm_read_byte(&sineTable[(angle <= 540) ?
      (angle - 360) : (720 - angle)]);
}

char fixCos(int angle) {
  angle %= 720;
  if(angle < 0) angle += 720;
  return (angle <= 360) ?
    ((angle <= 180) ?  pgm_read_byte(&sineTable[180 - angle])  :
                      -pgm_read_byte(&sineTable[angle - 180])) :
    ((angle <= 540) ? -pgm_read_byte(&sineTable[540 - angle])  :
                       pgm_read_byte(&sineTable[angle - 540]));
}

// ---------------------------------------------------------------------------
// Larry's Pattern Effects - Ported to FastLED/WS2812B
// ---------------------------------------------------------------------------

// Effect 0: Solid color fill
void renderEffect00(byte idx) {
  if(fxVars[idx][0] == 0) {
    byte *ptr = &imgData[idx][0];
    byte r = random(256), g = random(256), b = random(256);
    for(int i = 0; i < NUM_LEDS; i++) {
      *ptr++ = r; *ptr++ = g; *ptr++ = b;
    }
    fxVars[idx][0] = 1;
  }
}

// Effect 1: Rainbow (1 or more full loops of color wheel)
void renderEffect01(byte idx) {
  if(fxVars[idx][0] == 0) {
    fxVars[idx][1] = (1 + random(4 * ((NUM_LEDS + 31) / 32))) * 1536;
    fxVars[idx][2] = 4 + random(fxVars[idx][1]) / NUM_LEDS;
    if(random(2) == 0) fxVars[idx][1] = -fxVars[idx][1];
    if(random(2) == 0) fxVars[idx][2] = -fxVars[idx][2];
    fxVars[idx][3] = 0;
    fxVars[idx][0] = 1;
  }

  byte *ptr = &imgData[idx][0];
  long color, i;
  for(i = 0; i < NUM_LEDS; i++) {
    color = hsv2rgb(fxVars[idx][3] + fxVars[idx][1] * i / NUM_LEDS, 255, 255);
    *ptr++ = color >> 16; *ptr++ = color >> 8; *ptr++ = color;
  }
  fxVars[idx][3] += fxVars[idx][2];
}

// Effect 2: Sine wave chase
void renderEffect02(byte idx) {
  if(fxVars[idx][0] == 0) {
    fxVars[idx][1] = random(1536);  // Random hue
    fxVars[idx][2] = (1 + random(4 * ((NUM_LEDS + 31) / 32))) * 720;
    fxVars[idx][3] = 4 + random(fxVars[idx][1]) / NUM_LEDS;
    if(random(2) == 0) fxVars[idx][3] = -fxVars[idx][3];
    fxVars[idx][4] = 0;
    fxVars[idx][0] = 1;
  }

  byte *ptr = &imgData[idx][0];
  int  foo;
  long color, i;
  for(i = 0; i < NUM_LEDS; i++) {
    foo = fixSin(fxVars[idx][4] + fxVars[idx][2] * i / NUM_LEDS);
    color = (foo >= 0) ?
       hsv2rgb(fxVars[idx][1], 254 - (foo * 2), 255) :
       hsv2rgb(fxVars[idx][1], 255, 254 + foo * 2);
    *ptr++ = color >> 16; *ptr++ = color >> 8; *ptr++ = color;
  }
  fxVars[idx][4] += fxVars[idx][3];
}

// Flag colors - can customize to your preference
#define C_RED   160,   0,   0
#define C_WHITE 255, 255, 255
#define C_BLUE    0,   0, 100
PROGMEM const uint8_t flagTable[] = {
  C_BLUE , C_WHITE, C_BLUE , C_WHITE, C_BLUE , C_WHITE, C_BLUE,
  C_RED  , C_WHITE, C_RED  , C_WHITE, C_RED  , C_WHITE, C_RED ,
  C_WHITE, C_RED  , C_WHITE, C_RED  , C_WHITE, C_RED
};

// Effect 3: Wavy flag
void renderEffect03(byte idx) {
  long i, sum, s, x;
  int  idx1, idx2, a, b;
  if(fxVars[idx][0] == 0) {
    fxVars[idx][1] = 720 + random(720);  // Wavyness
    fxVars[idx][2] = 4 + random(10);     // Wave speed
    fxVars[idx][3] = 200 + random(200);  // Wave 'puckeryness'
    fxVars[idx][4] = 0;
    fxVars[idx][0] = 1;
  }

  for(sum = 0, i = 0; i < NUM_LEDS - 1; i++) {
    sum += fxVars[idx][3] + fixCos(fxVars[idx][4] + fxVars[idx][1] * i / NUM_LEDS);
  }

  byte *ptr = &imgData[idx][0];
  for(s = 0, i = 0; i < NUM_LEDS; i++) {
    x = 256L * ((sizeof(flagTable) / 3) - 1) * s / sum;
    idx1 =  (x >> 8)      * 3;
    idx2 = ((x >> 8) + 1) * 3;
    b    = (x & 255) + 1;
    a    = 257 - b;
    *ptr++ = ((pgm_read_byte(&flagTable[idx1    ]) * a) +
              (pgm_read_byte(&flagTable[idx2    ]) * b)) >> 8;
    *ptr++ = ((pgm_read_byte(&flagTable[idx1 + 1]) * a) +
              (pgm_read_byte(&flagTable[idx2 + 1]) * b)) >> 8;
    *ptr++ = ((pgm_read_byte(&flagTable[idx1 + 2]) * a) +
              (pgm_read_byte(&flagTable[idx2 + 2]) * b)) >> 8;
    s += fxVars[idx][3] + fixCos(fxVars[idx][4] + fxVars[idx][1] * i / NUM_LEDS);
  }

  fxVars[idx][4] += fxVars[idx][2];
  if(fxVars[idx][4] >= 720) fxVars[idx][4] -= 720;
}

// ---------------------------------------------------------------------------
// Larry's Alpha/Transition Effects
// ---------------------------------------------------------------------------

// Alpha 0: Simple fade
void renderAlpha00(void) {
  byte fade = 255L * tCounter / transitionTime;
  for(int i = 0; i < NUM_LEDS; i++) alphaMask[i] = fade;
}

// Alpha 1: Left-to-right wipe
void renderAlpha01(void) {
  long x, y, b;
  if(fxVars[2][0] == 0) {
    fxVars[2][1] = random(1, NUM_LEDS);
    fxVars[2][2] = (random(2) == 0) ? 255 : -255;
    fxVars[2][0] = 1;
  }

  b = (fxVars[2][2] > 0) ?
    (255L + (NUM_LEDS * fxVars[2][2] / fxVars[2][1])) *
      tCounter / transitionTime - (NUM_LEDS * fxVars[2][2] / fxVars[2][1]) :
    (255L - (NUM_LEDS * fxVars[2][2] / fxVars[2][1])) *
      tCounter / transitionTime;
  for(x = 0; x < NUM_LEDS; x++) {
    y = x * fxVars[2][2] / fxVars[2][1] + b;
    if(y < 0)         alphaMask[x] = 0;
    else if(y >= 255) alphaMask[x] = 255;
    else              alphaMask[x] = (byte)y;
  }
}

// Alpha 2: Dither reveal
void renderAlpha02(void) {
  long fade;
  int  i, bit, reverse, hiWord;

  if(fxVars[2][0] == 0) {
    int hiBit, n = (NUM_LEDS - 1) >> 1;
    for(hiBit = 1; n; n >>= 1) hiBit <<= 1;
    fxVars[2][1] = hiBit;
    fxVars[2][0] = 1;
  }

  for(i = 0; i < NUM_LEDS; i++) {
    for(reverse = 0, bit = 1; bit <= fxVars[2][1]; bit <<= 1) {
      reverse <<= 1;
      if(i & bit) reverse |= 1;
    }
    fade   = 256L * NUM_LEDS * tCounter / transitionTime;
    hiWord = (fade >> 8);
    if(reverse == hiWord)     alphaMask[i] = (fade & 255);
    else if(reverse < hiWord) alphaMask[i] = 255;
    else                      alphaMask[i] = 0;
  }
}

// ---------------------------------------------------------------------------
// Larry's Rendering Engine - Dual Buffer with Alpha Compositing
// ---------------------------------------------------------------------------

void updatePatterns() {
  byte frontImgIdx = 1 - backImgIdx;
  byte *backPtr = &imgData[backImgIdx][0];
  byte r, g, b;
  int i;

  // Always render back image
  (*renderEffect[fxIdx[backImgIdx]])(backImgIdx);

  // Front render and composite only during transitions
  if(tCounter > 0) {
    byte *frontPtr = &imgData[frontImgIdx][0];
    int  alpha, inv;

    // Render front image and alpha mask
    (*renderEffect[fxIdx[frontImgIdx]])(frontImgIdx);
    (*renderAlpha[fxIdx[2]])();

    // Composite front over back
    for(i = 0; i < NUM_LEDS; i++) {
      alpha = alphaMask[i] + 1;
      inv   = 257 - alpha;
      r = applyGamma((*frontPtr++ * alpha + *backPtr++ * inv) >> 8);
      g = applyGamma((*frontPtr++ * alpha + *backPtr++ * inv) >> 8);
      b = applyGamma((*frontPtr++ * alpha + *backPtr++ * inv) >> 8);
      leds[i].r = r;
      leds[i].g = g;
      leds[i].b = b;
    }
  } else {
    // No transition - just show back image
    for(i = 0; i < NUM_LEDS; i++) {
      r = applyGamma(*backPtr++);
      g = applyGamma(*backPtr++);
      b = applyGamma(*backPtr++);
      leds[i].r = r;
      leds[i].g = g;
      leds[i].b = b;
    }
  }

  // Count up to next transition
  tCounter++;
  if(tCounter == 0) {
    // Transition start
    fxIdx[frontImgIdx] = random(sizeof(renderEffect) / sizeof(renderEffect[0]));
    fxIdx[2]           = random(sizeof(renderAlpha)  / sizeof(renderAlpha[0]));
    transitionTime     = random(30, 181);  // 0.5 to 3 second transitions at 60 FPS
    fxVars[frontImgIdx][0] = 0;
    fxVars[2][0]           = 0;
  } else if(tCounter >= transitionTime) {
    // End transition
    fxIdx[backImgIdx] = fxIdx[frontImgIdx];
    backImgIdx        = 1 - backImgIdx;
    tCounter          = -120 - random(240);  // Hold 2-6 seconds at 60 FPS
  }
}

// ---------------------------------------------------------------------------
// Audio System (from m5lights_v1)
// ---------------------------------------------------------------------------

void initAudio() {
  // EXACT same approach as m5lights_v1 - NO config() calls!
  M5.Mic.begin();
  M5.Mic.setSampleRate(MIC_SR);
  lastBpmMillis = millis();
  Serial.println("Audio initialized (simple method)");
}

void detectAudioFrame() {
  static int16_t micBuf[MIC_BUF_LEN];
  static unsigned long lastDebug = 0;

  if (!M5.Mic.record(micBuf, MIC_BUF_LEN)) {
    if (millis() - lastDebug > 5000) {
      Serial.println("WARNING: Mic.record() failed");
      lastDebug = millis();
    }
    return;
  }

  long sum = 0;
  int16_t minVal = 32767, maxVal = -32768;
  for (auto &v : micBuf) {
    sum += abs(v);
    if (v < minVal) minVal = v;
    if (v > maxVal) maxVal = v;
  }
  float raw = float(sum) / MIC_BUF_LEN / 32767.0f;

  // Debug output every 5 seconds
  if (millis() - lastDebug > 5000) {
    Serial.print("Audio raw level: ");
    Serial.print(raw, 4);
    Serial.print(" (");
    Serial.print((int)(raw * 100));
    Serial.print("%) | Buffer range: ");
    Serial.print(minVal);
    Serial.print(" to ");
    Serial.print(maxVal);
    Serial.print(" | Peak-to-peak: ");
    Serial.println(maxVal - minVal);
    lastDebug = millis();
  }

  soundMin = min(raw, SMOOTH * soundMin + (1 - SMOOTH) * raw);
  soundMax = max(raw, SMOOTH * soundMax + (1 - SMOOTH) * raw);

  float dynamicRange = soundMax - soundMin;
  const float MIN_DYNAMIC_RANGE = 0.08f;
  const float HIGH_VOLUME_THRESHOLD = 0.7f;

  float adaptedMin = soundMin;
  float adaptedMax = soundMax;
  float beatThreshold = 0.6f;

  bool highVolumeEnvironment = (soundMin > HIGH_VOLUME_THRESHOLD) || (dynamicRange < MIN_DYNAMIC_RANGE);

  if (highVolumeEnvironment) {
    if (dynamicRange < MIN_DYNAMIC_RANGE) {
      float expansion = (MIN_DYNAMIC_RANGE - dynamicRange) * 0.5f;
      adaptedMin = max(0.0f, soundMin - expansion);
      adaptedMax = min(1.0f, soundMax + expansion);
    }
    beatThreshold = 0.35f;
  }

  musicLevel = constrain((raw - adaptedMin) / (adaptedMax - adaptedMin + 1e-6f), 0.0f, 1.0f);
  audioLevel = musicLevel;

  bool above = (musicLevel > beatThreshold);
  if (above && !prevAbove) {
    uint32_t t = millis();

    if (beatCount < 50) {
      beatTimes[beatCount++] = t;
    } else {
      memmove(beatTimes, beatTimes + 1, 49 * sizeof(uint32_t));
      beatTimes[49] = t;
    }

    if (lastBeatTime > 0) {
      uint32_t interval = t - lastBeatTime;
      if (interval >= 150 && interval <= 2000) {
        if (intervalCount < 50) {
          beatIntervals[intervalCount++] = interval;
        } else {
          memmove(beatIntervals, beatIntervals + 1, 49 * sizeof(uint32_t));
          beatIntervals[49] = interval;
        }
      }
    }
    lastBeatTime = t;
    beatDetected = true;
  } else if (!above) {
    beatDetected = false;
  }
  prevAbove = above;
}

uint32_t getMedianInterval() {
  if (intervalCount == 0) return 0;

  uint32_t temp[50];
  memcpy(temp, beatIntervals, intervalCount * sizeof(uint32_t));

  for (int i = 0; i < intervalCount - 1; i++) {
    for (int j = 0; j < intervalCount - i - 1; j++) {
      if (temp[j] > temp[j + 1]) {
        uint32_t swap = temp[j];
        temp[j] = temp[j + 1];
        temp[j + 1] = swap;
      }
    }
  }

  if (intervalCount % 2 == 0) {
    return (temp[intervalCount/2 - 1] + temp[intervalCount/2]) / 2;
  } else {
    return temp[intervalCount/2];
  }
}

void updateBPM() {
  uint32_t now = millis();
  if (now - lastBpmMillis >= BPM_WINDOW) {
    int cnt = 0;
    uint32_t cutoff = now - BPM_WINDOW;
    for (int i = 0; i < beatCount; i++) {
      if (beatTimes[i] >= cutoff) cnt++;
    }

    float bpm = 0.0f;
    if (intervalCount >= 3) {
      uint32_t medianInterval = getMedianInterval();
      if (medianInterval > 0) {
        bpm = 60000.0f / float(medianInterval);
      }
    } else if (cnt >= 2) {
      bpm = cnt * (60000.0f / float(BPM_WINDOW));
    }

    if (currentBPM == 0.0f || currentBPM < 10.0f) {
      currentBPM = bpm;
    } else if (bpm > 0.0f) {
      currentBPM = currentBPM * 0.9f + bpm * 0.1f;
    }

    Serial.print("BPM: beats=");
    Serial.print(cnt);
    Serial.print(", intervals=");
    Serial.print(intervalCount);
    Serial.print(", BPM=");
    Serial.print(currentBPM);
    Serial.print(", audio=");
    Serial.println(audioDetected ? "YES" : "NO");

    bool beatsDetected = (cnt >= 2 && currentBPM >= 30.0f && currentBPM <= 300.0f);
    bool sustainBeats = (cnt >= 1 && currentBPM >= 30.0f && currentBPM <= 300.0f);

    if (beatsDetected || sustainBeats) {
      lastMusicDetectedTime = now;
      audioDetected = true;
    } else {
      if (now - lastMusicDetectedTime > 20000) {
        audioDetected = false;
        currentBPM = 0.0f;
      }
    }

    lastBpmMillis += BPM_WINDOW;
    beatCount = 0;
  }
}

void updateAudioLevel() {
  detectAudioFrame();
  updateBPM();

  noiseFloor = noiseFloor * noiseFloorSmooth + audioLevel * (1.0f - noiseFloorSmooth);
  peakLevel = peakLevel * peakLevelSmooth + audioLevel * (1.0f - peakLevelSmooth);

  float range = peakLevel - noiseFloor;
  if (range < 0.01f) range = 0.01f;

  float normalizedLevel = (audioLevel - noiseFloor) / range;
  normalizedLevel = constrain(normalizedLevel, 0.0f, 1.0f);

  float targetBrightness;
  if (normalizedLevel < BRIGHTNESS_THRESHOLD) {
    targetBrightness = 1.0f;
  } else {
    float scaledLevel = (normalizedLevel - BRIGHTNESS_THRESHOLD) / (1.0f - BRIGHTNESS_THRESHOLD);
    float curved = pow(scaledLevel, BRIGHTNESS_POWER_CURVE);
    targetBrightness = 1.0f + (curved * 24.0f);
  }

  unsigned long now = millis();
  float timeDelta = (now - lastBrightnessUpdate) / 1000.0f;
  lastBrightnessUpdate = now;

  if (targetBrightness > brightnessEnvelope) {
    brightnessEnvelope = targetBrightness;
  } else {
    float decayFactor = exp(-timeDelta / BRIGHTNESS_DECAY_SECONDS);
    brightnessEnvelope = targetBrightness + (brightnessEnvelope - targetBrightness) * decayFactor;
  }

  musicBrightness = (uint8_t)brightnessEnvelope;
}

// ---------------------------------------------------------------------------
// ESP-NOW System (from m5lights_v1)
// ---------------------------------------------------------------------------

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("ESP-NOW: Send FAIL");
  }
}

void onDataReceived(const esp_now_recv_info* recv_info, const uint8_t *incomingData, int len) {
  unsigned long now = millis();

  if (currentMode == MODE_NORMAL_LEADER || currentMode == MODE_MUSIC_LEADER) {
    return;
  }

  if (len != sizeof(LEDSync)) {
    return;
  }

  LEDSync receivedData;
  memcpy(&receivedData, incomingData, sizeof(receivedData));

  bool wasActive = leaderDataActive;
  lastLeaderMessage = millis();
  leaderDataActive = true;

  if (!wasActive) {
    Serial.println(">>> LEADER DETECTED - now following <<<");
  }

  if (rejoinMode) {
    rejoinMode = false;
    rejoinAttempts = 0;
  }

  FastLED.setBrightness(receivedData.brightness);

  for (int i = 0; i < receivedData.count && i < 49; i++) {
    int ledIndex = receivedData.startIndex + i;
    if (ledIndex < NUM_LEDS) {
      int dataIndex = i * 3;
      leds[ledIndex].r = receivedData.rgbData[dataIndex];
      leds[ledIndex].g = receivedData.rgbData[dataIndex + 1];
      leds[ledIndex].b = receivedData.rgbData[dataIndex + 2];
    }
  }

  if (receivedData.startIndex + receivedData.count >= NUM_LEDS) {
    FastLED.show();
    lastCompleteFrame = millis();
  }
}

void setupESPNOW() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ESP-NOW peer add failed");
  } else {
    Serial.println("ESP-NOW setup complete");
  }
}

void broadcastLEDData() {
  const int LEDS_PER_PACKET = 49;
  static uint8_t sequenceNum = 0;

  LEDSync message;
  message.sequenceNum = sequenceNum++;

  for (int startIdx = 0; startIdx < NUM_LEDS; startIdx += LEDS_PER_PACKET) {
    message.startIndex = startIdx;
    message.count = min(LEDS_PER_PACKET, NUM_LEDS - startIdx);
    message.brightness = FastLED.getBrightness();

    for (int i = 0; i < message.count; i++) {
      int ledIdx = startIdx + i;
      int dataIdx = i * 3;
      message.rgbData[dataIdx] = leds[ledIdx].r;
      message.rgbData[dataIdx + 1] = leds[ledIdx].g;
      message.rgbData[dataIdx + 2] = leds[ledIdx].b;
    }

    esp_now_send(broadcastAddress, (uint8_t*)&message, sizeof(message));
    delayMicroseconds(500);
  }
}

// ---------------------------------------------------------------------------
// Display and Button Handling (from m5lights_v1)
// ---------------------------------------------------------------------------

void updateDisplay() {
  M5.Lcd.fillScreen(TFT_BLACK);

  // Set background color based on mode
  uint16_t bgColor = TFT_GREEN;
  const char* modeStr = "Normal";

  if (leaderDataActive && (currentMode == MODE_NORMAL || currentMode == MODE_MUSIC)) {
    bgColor = TFT_BLUE;
    modeStr = "Following";
  } else {
    switch(currentMode) {
      case MODE_NORMAL:        bgColor = TFT_GREEN;  modeStr = "Normal"; break;
      case MODE_MUSIC:         bgColor = TFT_PURPLE; modeStr = "Music"; break;
      case MODE_NORMAL_LEADER: bgColor = TFT_ORANGE; modeStr = "Leader"; break;
      case MODE_MUSIC_LEADER:  bgColor = TFT_RED;    modeStr = "Music Leader"; break;
    }
  }

  M5.Lcd.fillRect(0, 0, 240, 135, bgColor);
  M5.Lcd.setTextColor(TFT_WHITE, bgColor);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.printf("v%s.%d", VERSION, BUILD);

  M5.Lcd.setCursor(10, 35);
  M5.Lcd.print("Mode: ");
  M5.Lcd.println(modeStr);

  if (!leaderDataActive || currentMode == MODE_NORMAL_LEADER || currentMode == MODE_MUSIC_LEADER) {
    M5.Lcd.setCursor(10, 60);
    M5.Lcd.print("Pattern: ");
    M5.Lcd.println(patternNames[fxIdx[backImgIdx]]);
  }

  if (currentMode == MODE_MUSIC || currentMode == MODE_MUSIC_LEADER) {
    M5.Lcd.setCursor(10, 85);
    M5.Lcd.printf("Audio: %d%%", (int)(audioLevel * 100));
    M5.Lcd.setCursor(10, 110);
    M5.Lcd.printf("BPM: %.0f  Beat: %s", currentBPM, audioDetected ? "YES" : "NO");
  }
}

void handleButtons() {
  M5.update();

  static unsigned long btnAPressed = 0;
  static unsigned long btnBPressed = 0;
  static bool btnAHandled = false;
  static bool btnBHandled = false;

  // Button A: Mode switching
  if (M5.BtnA.wasPressed()) {
    btnAPressed = millis();
    btnAHandled = false;
  }

  if (M5.BtnA.isPressed() && !btnAHandled) {
    if (millis() - btnAPressed >= LONG_PRESS_TIME_MS) {
      // Long press: Toggle leader status
      if (currentMode == MODE_NORMAL) currentMode = MODE_NORMAL_LEADER;
      else if (currentMode == MODE_MUSIC) currentMode = MODE_MUSIC_LEADER;
      else if (currentMode == MODE_NORMAL_LEADER) currentMode = MODE_NORMAL;
      else if (currentMode == MODE_MUSIC_LEADER) currentMode = MODE_MUSIC;
      btnAHandled = true;
      updateDisplay();
    }
  }

  if (M5.BtnA.wasReleased() && !btnAHandled) {
    // Short press: Toggle Normal/Music
    if (currentMode == MODE_NORMAL) currentMode = MODE_MUSIC;
    else if (currentMode == MODE_MUSIC) currentMode = MODE_NORMAL;
    else if (currentMode == MODE_NORMAL_LEADER) currentMode = MODE_MUSIC_LEADER;
    else if (currentMode == MODE_MUSIC_LEADER) currentMode = MODE_NORMAL_LEADER;
    updateDisplay();
  }

  // Button B: Manual pattern advance (disabled in follow mode)
  if (M5.BtnB.wasPressed() && !leaderDataActive) {
    // Trigger immediate transition
    if (tCounter < 0) tCounter = 0;
  }
}

// ---------------------------------------------------------------------------
// Setup and Main Loop
// ---------------------------------------------------------------------------

void setup() {
  // Initialize M5 with config (like m5lights_v1)
  auto cfg = M5.config();
  M5.begin(cfg);

  // Initialize display
  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(TFT_BLACK);

  // Initialize serial with delay (like m5lights_v1)
  Serial.begin(115200);
  delay(1000);

  Serial.println("================================");
  Serial.print("M5HeadBand v"); Serial.print(VERSION);
  Serial.print(" Build "); Serial.println(BUILD);
  Serial.println("Initializing...");
  Serial.println("================================");

  // Initialize watchdog (ESP32 v3.x API)
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // Initialize audio BEFORE FastLED (critical!)
  initAudio();
  lastBrightnessUpdate = millis();

  // Initialize ESP-NOW
  setupESPNOW();

  // Initialize FastLED (after audio)
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
    .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  // Initialize pattern system
  randomSeed(analogRead(0));
  memset(imgData, 0, sizeof(imgData));
  fxVars[backImgIdx][0] = 1;  // Mark back image as initialized

  updateDisplay();

  Serial.println("M5HeadBand Ready!");
}

void loop() {
  static unsigned long lastFrameTime = 0;
  unsigned long now = millis();

  // Feed watchdog
  esp_task_wdt_reset();

  // Handle buttons
  handleButtons();

  // Check for leader timeout (follower mode)
  if (leaderDataActive) {
    if (now - lastLeaderMessage > LEADER_TIMEOUT_MS) {
      Serial.println("Leader timeout - returning to standalone");
      leaderDataActive = false;
      updateDisplay();
    }
  }

  // Update at 60 FPS
  if (now - lastFrameTime >= (1000 / FRAMES_PER_SECOND)) {
    lastFrameTime = now;

    // Update audio in music modes
    if (currentMode == MODE_MUSIC || currentMode == MODE_MUSIC_LEADER) {
      updateAudioLevel();
      FastLED.setBrightness(musicBrightness);
    } else {
      FastLED.setBrightness(BRIGHTNESS);
    }

    // Render patterns (unless following)
    if (!leaderDataActive) {
      updatePatterns();

      // Leader modes: broadcast then show
      if (currentMode == MODE_NORMAL_LEADER || currentMode == MODE_MUSIC_LEADER) {
        if (now - lastBroadcast >= BROADCAST_INTERVAL_MS) {
          broadcastLEDData();
          lastBroadcast = now;
        }
        delay(LEADER_DELAY_MS);
      }

      FastLED.show();
    }
  }

  // Update display periodically (less frequent to avoid tearing)
  if (now - lastDisplayUpdate > 2000) {
    updateDisplay();
    lastDisplayUpdate = now;
  }
}

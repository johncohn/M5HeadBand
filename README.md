# M5HeadBand v1.0.0

LED Headband controller combining Larry's smooth parametric pattern system with ESP-NOW synchronization and music reactivity for M5StickC Plus 2.

## Overview

M5HeadBand merges two powerful LED control systems:

- **Larry's Pattern System** (from larry_v3.ono): Dual-buffer architecture with alpha channel compositing for ultra-smooth pattern transitions
- **m5lights Infrastructure**: ESP-NOW wireless sync, music reactivity, beat detection, and multi-device leader/follower modes

## Key Features

### Larry's Pattern System

- **Dual Buffer Rendering**: Front and back image buffers for seamless transitions
- **Alpha Channel Compositing**: Smooth cross-fades between patterns using parametric alpha effects
- **Parametric Effects**: Procedurally generated patterns with state-based randomization
- **4 Visual Effects**:
  - Solid Color - Random solid colors
  - Rainbow - Scrolling rainbow with variable loops and speeds
  - Sine Wave - Pulsing sine wave chase with hue variations
  - Wavy Flag - Flag colors with organic wave motion
- **3 Transition Effects**:
  - Fade - Simple cross-fade
  - Wipe - Left-to-right or right-to-left wipe
  - Dither - Ordered dither reveal
- **Smart Timing**: Random transition durations (0.5-3 seconds), pattern hold times (2-6 seconds)

### Music Reactivity

- **Beat Detection**: Real-time beat detection with BPM calculation
- **Interval-Based BPM**: Uses median beat intervals for stable tempo tracking
- **Adaptive Audio**: Auto-adjusts to quiet and loud environments
- **Brightness Decay Envelope**: Fast attack, smooth exponential decay for natural response
- **Power Curve**: Prevents over-reaction to background noise
- **Display**: Shows audio level percentage, BPM, and beat detection status

### ESP-NOW Synchronization

- **Operating Modes**:
  - 🟢 **Normal Mode** - Standalone patterns
  - 🟣 **Music Mode** - Music-reactive standalone
  - 🟠 **Normal Leader** - Broadcasts pattern data to followers
  - 🔴 **Music Leader** - Broadcasts music-reactive patterns
  - 🔵 **Follow Mode** - Automatically syncs with detected leader

- **Zero-Lag Sync**: Followers mirror leader's exact LED states
- **Automatic Discovery**: Followers auto-detect and follow leaders
- **Robust Timeout Handling**: 8-second timeout with automatic fallback

## Hardware Setup

- **Device**: M5StickC Plus 2
- **LED Strip**: WS2811/WS2812B compatible (200 LEDs)
- **Connection**: LED data wire to GPIO 32
- **Power**: Adequate 5V supply for LED strip

## Controls

### Button A (Main Button)
- **Short Press**: Toggle Normal ↔ Music mode
- **Long Press (1.5s)**: Toggle Leader status

### Button B (Side Button)
- **Short Press**: Manually advance to next pattern (disabled in Follow mode)

## Pattern System Architecture

### Dual Buffer Concept

The system maintains two complete LED strip images (back and front) at all times:

1. **Active Pattern (Back Image)**: Currently displayed pattern
2. **Incoming Pattern (Front Image)**: Next pattern being transitioned to
3. **Alpha Mask**: Controls blend between front and back (0-255 per LED)

### Transition Process

```
Hold Phase (2-6 seconds)
  ↓
Transition Start (random effect & alpha selected)
  ↓
Transition Active (0.5-3 seconds)
  - Back pattern continues rendering
  - Front pattern renders new effect
  - Alpha effect controls blend
  - Composite = (front × alpha) + (back × (1-alpha))
  ↓
Transition Complete
  - Front becomes new back
  - Buffers swap
  - Return to hold phase
```

### Porting from LPD8806 to WS2812B

Key conversions made:
- LED library: LPD8806 → FastLED
- Color depth: 7-bit → 8-bit
- Gamma table: Updated for 8-bit output
- Timing: Timer1 interrupts → 60 FPS loop-based
- Scale: 20 LEDs → 200 LEDs
- Platform: Arduino → ESP32 M5Stack

## Configuration

```cpp
// Hardware
#define LED_PIN 32
#define NUM_LEDS 200
#define BRIGHTNESS 25
#define CHIPSET WS2811
#define COLOR_ORDER GRB
#define FRAMES_PER_SECOND 60

// Audio
#define MIC_BUF_LEN 240
#define MIC_SR 44100
#define BRIGHTNESS_DECAY_SECONDS 0.25f
#define BRIGHTNESS_THRESHOLD 0.35f
#define BRIGHTNESS_POWER_CURVE 2.0f

// Timing
#define LONG_PRESS_TIME_MS 1500
#define BROADCAST_INTERVAL_MS 50
#define LEADER_TIMEOUT_MS 8000
```

## Installation

1. **Install Required Libraries** (Arduino IDE or PlatformIO):
   - M5StickCPlus2
   - FastLED
   - M5Unified
   - M5GFX
   - ESP-NOW (built-in)
   - WiFi (built-in)

2. **Select Board**: M5Stack → M5StickC Plus 2

3. **Upload**: Flash M5HeadBand.ino to your M5StickC Plus 2

4. **Connect Hardware**:
   - LED strip data → GPIO 32 (Grove white wire)
   - LED strip GND → GND (Grove black wire)
   - LED strip power → External 5V supply

## Multi-Device Setup

### 2+ Device Synchronized Show

1. **Setup Leader**:
   - Upload code to first M5Stick
   - Choose mode (Normal or Music)
   - Long press Button A → becomes Leader (Orange/Red)

2. **Setup Followers**:
   - Upload code to additional M5Sticks
   - Power on → they auto-detect Leader
   - Display turns Blue ("Following")
   - Patterns sync automatically

3. **Music Sync**:
   - Put Leader in Music Leader mode (Red)
   - Position near music source
   - All followers sync brightness and patterns to Leader's microphone

## Technical Details

### Memory Usage

- **Dual Buffers**: 2 × 200 LEDs × 3 bytes = 1,200 bytes
- **Alpha Mask**: 200 bytes
- **State Variables**: 3 × 50 integers = 300 bytes
- **FastLED Buffer**: 200 × 3 bytes = 600 bytes
- **Total Pattern System**: ~2.3 KB

### ESP-NOW Protocol

- **Broadcast**: FF:FF:FF:FF:FF:FF (one-to-many)
- **Update Rate**: 50ms (20 Hz)
- **Packet Size**: 153 bytes (49 LEDs + metadata)
- **Packets Per Frame**: 5 (for 200 LEDs)
- **Channel**: WiFi channel 1

### Audio Processing

- **Sample Rate**: 44.1 kHz
- **Buffer**: 240 samples/frame
- **Beat Detection**: Threshold-based with hysteresis
- **BPM Calculation**: Median interval filtering
- **Range**: 30-300 BPM valid
- **Sticky Detection**: 20-second timeout for music mode

## Differences from m5lights_v1

### Replaced
- ❌ 14 discrete FastLED patterns → 4 parametric patterns
- ❌ Simple cross-fade → Dual-buffer alpha compositing
- ❌ Manual pattern list → Randomized effect selection

### Added
- ✅ Dual buffer architecture
- ✅ Alpha channel transitions (fade, wipe, dither)
- ✅ Parametric effect generation
- ✅ State-based pattern variations
- ✅ Gamma correction for smoother gradients

### Kept
- ✅ All ESP-NOW sync functionality
- ✅ All operating modes (Normal/Music/Leader)
- ✅ Beat detection and BPM calculation
- ✅ Sound-reactive brightness
- ✅ Button handling and display
- ✅ Leader/Follower architecture

## Credits

- **Larry's Pattern System**: Original larry_v3.ono concepts and algorithms
- **m5lights_v1**: ESP-NOW sync and music reactivity by John Cohn, 2024
- **FastLED Patterns**: Mark Kriegsman
- **M5HeadBand Integration**: John Cohn, 2024

## Version History

### v1.0.0 (2024-11-30) - Initial Release
- Ported larry_v3.ono dual-buffer transition system
- Ported 4 parametric effects: Solid, Rainbow, Sine Wave, Wavy Flag
- Ported 3 alpha transitions: Fade, Wipe, Dither
- Kept m5lights_v1 ESP-NOW sync (Normal/Music/Leader modes)
- Kept m5lights_v1 beat detection and BPM display
- Kept m5lights_v1 sound-reactive brightness with decay envelope
- Converted from LPD8806 (7-bit) to WS2812B (8-bit) LEDs
- Converted from Timer1 interrupts to loop-based rendering
- Scaled from 20 LEDs to 200 LEDs
- Beat detection active but patterns don't respond to beats yet

## Future Enhancements

- [ ] Add beat-responsive pattern variations
- [ ] Add more parametric effects (sparkle, plasma, fire)
- [ ] Add more alpha transitions (radial, center-out)
- [ ] Pattern selection via button controls
- [ ] Save/load pattern preferences
- [ ] Web interface for configuration

## License

Open source - feel free to modify and share!

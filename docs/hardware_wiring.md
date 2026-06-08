# Hardware Wiring

This document describes the current physical wiring for the STM32F407 ambient
lighting prototype.

## Power Domains

The system uses two power paths:

- The STM32F407 Discovery board is powered through ST-LINK/USB.
- The WS2812 strip and SN74AHCT125 are powered by a regulated external 5 V
  supply.

All grounds must be connected together so the 3.3 V STM32 signal and 5 V
level-shifter output share the same reference.

```text
STM32 GND -------------------+
TFP401 GND ------------------+---- Common GND
SN74AHCT125 pin 7 -----------+
WS2812 GND ------------------+
5 V power-supply negative ---+
```

Do not power the full WS2812 strip from the STM32 board.

## WS2812 Data Path

```text
STM32 PA1 / TIM2_CH2
        |
        v
SN74AHCT125 pin 2 / 1A
        |
SN74AHCT125 pin 3 / 1Y
        |
     330 ohm
        |
        v
WS2812 DIN
```

PA1 produces the 800 kHz timer-PWM waveform. The SN74AHCT125 accepts the
STM32's 3.3 V logic level and reproduces it as a non-inverted 5 V signal.

## SN74AHCT125 Channel 1 Wiring

The notch or orientation mark identifies the top of the DIP-14 package. Pin
numbers increase counter-clockwise when viewed from above.

| SN74AHCT125 pin | Signal | Connection |
|---:|---|---|
| 1 | `/1OE` | Common GND; active-low output enable |
| 2 | `1A` | STM32 PA1 / TIM2_CH2 |
| 3 | `1Y` | 330 ohm series resistor, then WS2812 DIN |
| 7 | GND | Common GND |
| 14 | VCC | Regulated 5 V supply |

Unused channel inputs and output-enable pins should not be left floating in a
permanent build. Tie unused output-enable pins high to 5 V to disable those
channels, and tie unused inputs to a defined logic level.

## Capacitor Placement

Both capacitors connect across the same 5 V and GND rails, but they serve
different purposes and should both be installed.

### 100 nF Ceramic Decoupling Capacitor

Connect the 100 nF (`0.1 uF`) ceramic capacitor directly between:

```text
SN74AHCT125 pin 14 / VCC ----||---- SN74AHCT125 pin 7 / GND
```

Place it physically close to pins 14 and 7. A ceramic capacitor is
non-polarized, so either lead may connect to VCC or GND.

### 100 uF Electrolytic Bulk Capacitor

Connect the 100 uF, 16 V electrolytic capacitor across the external 5 V supply
near the level shifter and LED-strip input:

```text
5 V supply positive -------- (+ 100 uF -) -------- Common GND
```

Electrolytic capacitors are polarized:

- Positive lead connects to 5 V.
- Negative lead, usually marked with a stripe, connects to GND.

The 100 nF capacitor handles fast local switching transients. The 100 uF
capacitor supports slower load changes on the 5 V rail.

## Series Resistor

Place the 330 ohm resistor between SN74AHCT125 pin 3 (`1Y`) and WS2812 DIN,
preferably close to the first LED. It reduces ringing and limits transient
current on the data line.

## LED Power Wiring

```text
External regulated 5 V positive ---- WS2812 +5V
External regulated 5 V negative ---- WS2812 GND
External regulated 5 V positive ---- SN74AHCT125 pin 14
External regulated 5 V negative ---- SN74AHCT125 pin 7
```

Use appropriately sized power wiring for the strip. Begin testing at low
brightness because WS2812 current demand rises quickly with brightness and LED
count.

## Complete Level-Shifter Wiring Checklist

- [ ] SN74AHCT125 pin 14 connected to regulated 5 V
- [ ] SN74AHCT125 pin 7 connected to common GND
- [ ] SN74AHCT125 pin 1 connected to common GND
- [ ] STM32 PA1 connected to SN74AHCT125 pin 2
- [ ] SN74AHCT125 pin 3 connected through 330 ohm to WS2812 DIN
- [ ] 100 nF ceramic capacitor directly across pins 14 and 7
- [ ] 100 uF electrolytic capacitor across 5 V and GND with correct polarity
- [ ] STM32, TFP401, level shifter, LED strip, and 5 V supply grounds connected
- [ ] LED strip connected at DIN, with arrows pointing away from the controller

## Signal Validation

With a logic analyzer or oscilloscope:

1. Probe PA1 to confirm a 3.3 V, 800 kHz WS2812 waveform.
2. Probe SN74AHCT125 pin 3 to confirm the same non-inverted waveform at
   approximately 5 V.
3. Probe after the 330 ohm resistor at WS2812 DIN.
4. Confirm the line remains low between frames for the WS2812 reset/latch
   interval.


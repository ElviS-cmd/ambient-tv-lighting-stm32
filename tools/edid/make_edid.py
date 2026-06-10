#!/usr/bin/env python3
"""Generate the custom TFP401 EDID: tfp401_1280x720_50_rb.bin

EDID 1.3, DVI-only (no CEA extension -> sources emit plain DVI signaling,
which the DVI-only TFP401 can hold lock on), with a single preferred mode:

    1280x720 @ 49.94 Hz, CVT reduced blanking
    Pixel clock 53.00 MHz  (inside the STM32F407 DCMI's 54 MHz limit)
    H: 1280 / fp 48 / sync 32 / bp 80  (total 1440), HSync positive
    V:  720 / fp  3 / sync  5 / bp  9  (total  737), VSync negative

Write the output to the Adafruit TFP401 breakout's EDID EEPROM per the
Adafruit TFP401 guide (release the write-protect pad first), e.g. with
edid-rw on a Raspberry Pi or any Linux box exposing the DDC i2c bus.
"""

edid = bytearray(128)
edid[0:8] = bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
edid[8:10] = bytes([0x04, 0x81])              # Manufacturer 'ADA' (Adafruit)
edid[10:12] = (0x0720).to_bytes(2, 'little')  # product code
edid[12:16] = (1).to_bytes(4, 'little')       # serial
edid[16] = 1                                  # week
edid[17] = 36                                 # 1990 + 36 = 2026
edid[18:20] = bytes([1, 3])                   # EDID 1.3
edid[20] = 0x80                               # digital input
edid[21] = 0x0F
edid[22] = 0x0A                               # 15 x 10 cm
edid[23] = 0x78                               # gamma 2.2
edid[24] = 0x02                               # first DTD is preferred
edid[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
edid[35:38] = bytes([0x00, 0x00, 0x00])       # no established timings
edid[38:54] = bytes([0x01, 0x01] * 8)         # no standard timings

dtd = bytearray(18)
dtd[0:2] = (5300).to_bytes(2, 'little')       # 53.00 MHz in 10 kHz units
dtd[2] = 1280 & 0xFF                          # H active low byte
dtd[3] = 160 & 0xFF                           # H blank low byte
dtd[4] = ((1280 >> 8) << 4) | (160 >> 8)
dtd[5] = 720 & 0xFF                           # V active low byte
dtd[6] = 17 & 0xFF                            # V blank low byte
dtd[7] = ((720 >> 8) << 4) | (17 >> 8)
dtd[8] = 48                                   # H front porch
dtd[9] = 32                                   # H sync width
dtd[10] = (3 << 4) | 5                        # V front porch / V sync width
dtd[11] = 0                                   # porch/sync high bits
dtd[12] = 0x6C
dtd[13] = 0x44
dtd[14] = 0x00                                # image size (cosmetic)
dtd[15] = 0
dtd[16] = 0                                   # no border
dtd[17] = 0x1A                                # digital separate, HS+, VS-
edid[54:72] = dtd

name = b'AMBILIGHT'
edid[72:90] = bytes([0, 0, 0, 0xFC, 0]) + name + b'\x0A' + b' ' * (13 - len(name) - 1)
# Range limits: 23-76 Hz vertical, 30-46 kHz horizontal, max 60 MHz
edid[90:108] = bytes([0, 0, 0, 0xFD, 0, 23, 76, 30, 46, 6,
                      0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20])
edid[108:126] = bytes([0, 0, 0, 0x10, 0] + [0] * 13)  # dummy descriptor
edid[126] = 0                                 # no extension blocks
edid[127] = (256 - sum(edid[:127]) % 256) % 256

with open("tfp401_1280x720_50_rb.bin", "wb") as f:
    f.write(edid)

print("wrote tfp401_1280x720_50_rb.bin, checksum 0x%02X (sum %% 256 = %d)"
      % (edid[127], sum(edid) % 256))

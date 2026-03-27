## Keymap

![Keymap](./keymap-drawer/splitkb_aurora_corne.svg?v=1)

---

## Build & Flash

### Hardware

| Component | Details |
|-----------|---------|
| Board | Waveshare RP2040 Zero (×2) |
| Matrix | 4 rows × 6 columns per half |
| Col GPIOs | GPIO4, GPIO5, GPIO6, GPIO7, GPIO27, GPIO28 |
| Row GPIOs | GPIO0, GPIO1, GPIO2, GPIO3 (active-high, pull-down) |
| UART Split | TX = GPIO8, RX = GPIO9 (UART1, 115200 baud) |

**UART wiring between halves** (connect when right half is ready):
```
Left GPIO8 (TX)  →  Right GPIO9 (RX)
Left GPIO9 (RX)  →  Right GPIO8 (TX)
GND              →  GND
```

---

### Prerequisites

```bash
# Install west (Zephyr meta-tool) if not already installed
pip3 install west

# Initialise the ZMK workspace (run once)
west init -l config
west update
west zephyr-export
```

---

### Option A – Standalone left half (recommended for first test)

Use this to verify that all left-side keys work before the right half is built.

```bash
# Build
west build -p -s zmk/app -b waveshare_rp2040_zero \
  -- -DSHIELD="handwired" \
     -DZMK_CONFIG="$(pwd)/config"

# Firmware is at:  build/zephyr/zmk.uf2
```

Flash:
1. Hold the **BOOT** button on the RP2040 Zero while connecting USB.
2. A mass-storage drive (`RPI-RP2`) appears on your computer.
3. Copy `build/zephyr/zmk.uf2` onto that drive.
4. The board reboots and registers as a USB keyboard.

---

### Option B – Wired split (both halves)

#### Left half (central – connects to host via USB)

```bash
west build -p -s zmk/app -b waveshare_rp2040_zero \
  -- -DSHIELD="handwired_left" \
     -DZMK_CONFIG="$(pwd)/config"

cp build/zephyr/zmk.uf2 left.uf2
```

#### Right half (peripheral – connected to left via UART)

```bash
west build -p -s zmk/app -b waveshare_rp2040_zero \
  -- -DSHIELD="handwired_right" \
     -DZMK_CONFIG="$(pwd)/config"

cp build/zephyr/zmk.uf2 right.uf2
```

Flash both boards using the BOOT-button method described above.

---

### CI / GitHub Actions

Every push triggers the `ZMK Wired Split Build` workflow.  
Download the firmware from the **Actions** tab → latest run → **rp2040-handwired-firmware** artifact.  
The artifact contains three UF2 files:

| File | Use |
|------|-----|
| `handwired-standalone-left.uf2` | Left half alone (testing) |
| `handwired-split-left.uf2` | Left half in split mode (central) |
| `handwired-split-right.uf2` | Right half in split mode (peripheral) |

---

### Shield / config file overview

```
config/
├── handwired.keymap                  ← shared keymap (both halves)
├── west.yml                          ← ZMK dependency manifest
└── boards/shields/handwired/
    ├── Kconfig.defconfig             ← keyboard name / split-role defaults
    ├── Kconfig.shield                ← shield detection
    ├── handwired.overlay             ← standalone left-half DTS
    ├── handwired.conf                ← standalone left-half config
    ├── handwired_left.overlay        ← split central DTS (UART1 on GPIO8/9)
    ├── handwired_left.conf           ← split central config
    ├── handwired_right.overlay       ← split peripheral DTS (UART1 on GPIO8/9)
    └── handwired_right.conf          ← split peripheral config
```

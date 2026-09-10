# ESP-NOW Door Alarm (XIAO ESP32C6 Sender + ESP32S3 Receiver)

Arduino IDE sketches for a battery-powered wireless door/motion alarm with ESP-NOW.

## Sketch variants

### 1) Constant receiver mode
- `esp32-espnow-doorbell/constant/sender.ino`  
  **Board:** XIAO ESP32C6 Tiny SuperMini  
  PIR-triggered deep-sleep sender. Wakes on motion, reads battery, sends ESP-NOW packet(s), returns to deep sleep.
- `esp32-espnow-doorbell/constant/receiver.ino`  
  **Board:** ESP32S3 N16R8 DevKit  
  Always-listening receiver. On packet: 5s white LED blink alarm + buzzer grouped beep pattern.  
  Low-battery warning is a separate red blink pattern and is cleared when a later packet reports battery > 30%.

### 2) Low-power receiver mode (cooler operation)
- `esp32-espnow-doorbell/lowpower/sender.ino`  
  **Board:** XIAO ESP32C6 Tiny SuperMini  
  Sends a short packet burst per trigger (for better capture by duty-cycled receiver), then deep sleep.
- `esp32-espnow-doorbell/lowpower/receiver.ino`  
  **Board:** ESP32S3 N16R8 DevKit  
  Duty-cycled ESP-NOW receive (radio ON/OFF windows) to reduce heat/power.  
  Keeps the same user-visible alarm behavior as constant mode, plus a **60s alarm cooldown** after each trigger.

---

## Current behavior summary

### Alarm behavior (receiver)
- **Normal alarm (highest priority):** white LED blinking for 5 seconds.
- **Buzzer:** separate grouped beep pattern (not tied to LED blink rhythm).
- **Low-battery warning:** red double-blink + pause pattern, shown when sender reports low battery.
- **Low-battery clear rule:** warning clears once a newer packet reports battery > 30%.
- **BOOT button on receiver:**
  - If battery warning is active/pending: clears warning.
  - Otherwise: runs a local test alarm.

### Cooldown behavior (lowpower receiver)
- After a received alarm trigger, retriggering is blocked for **60 seconds**.
- Battery status updates are still processed during cooldown.

---

## ESP-NOW transport model

- Sender uses ESP-NOW **broadcast** (`FF:FF:FF:FF:FF:FF`), so no fixed peer MAC setup is required.
- Sender and receiver must be on the same Wi-Fi channel (`ESPNOW_CHANNEL`, currently `1`).
- In low-power mode, receiver duty-cycles the radio, so sender uses a short burst to improve catch probability.

---

## Battery reporting

Battery data is included in normal sender packets:
- `batteryMilliVolts`
- `batteryPercent`
- `lowBattery` flag

### Sender battery measurement notes
- Use a resistor divider to ADC input.
- Configure:
  - `BATTERY_DIVIDER_RATIO`
  - `BATTERY_EMPTY_MILLIVOLTS`
  - `BATTERY_FULL_MILLIVOLTS`
  - `BATTERY_LOW_PERCENT`

---

## Hardware mapping used now

### Sender
- **Board:** XIAO ESP32C6 Tiny SuperMini
- PIR input: `GPIO_NUM_2`
- Battery ADC pin: `0` (board mapping dependent)
- Onboard LED: GPIO15 (kept off in normal operation)

### Receiver
- **Board:** ESP32S3 N16R8 DevKit
- NeoPixel: GPIO48
- BOOT button: GPIO0
- Buzzer: GPIO16

---

## Why lowpower feels cooler

The lowpower receiver turns Wi-Fi/ESP-NOW RX on only in short windows instead of running continuously.  
That lowers average RF/baseband activity and reduces heat at the receiver board.

---

## Quick setup

1. Flash one sender sketch to XIAO ESP32C6:
   - `constant/sender.ino` or `lowpower/sender.ino`
2. Flash matching receiver sketch to ESP32S3:
   - `constant/receiver.ino` or `lowpower/receiver.ino`
3. Open Serial Monitor (115200) on both sides.
4. Trigger motion on sender PIR and verify alarm + battery logs on receiver.

---

## Notes

- If you want maximum reliability/range, increase sender burst count/interval (costs sender battery).
- If you want even lower receiver power, increase RX off-time (may require larger sender burst).
- If desired, add sequence dedupe on receiver to avoid repeated restarts from burst packets.

# ESP-NOW Deep-Sleep Pushbutton Signal (ESP32S3 Zero -> ESP32S3 DevBoard)

Arduino IDE sketches for a battery-powered wireless "call button":

- **sender/sender.ino** — runs on the ESP32S3 Zero. Sleeps in deep sleep to
  save battery. Pressing the BOOT button (GPIO0) wakes it, it reads its
  battery voltage, sends one ESP-NOW broadcast packet (button press +
  battery info), waits for the button release, then goes straight back
  to deep sleep.
- **receiver/receiver.ino** — runs on the ESP32S3 devboard (HW-678),
  powered continuously over USB. Turns the onboard WS2812 RGB LED red
  when it receives a packet, and turns it off only when its own BOOT
  button is pressed. Also logs the sender's battery voltage/percentage
  to Serial on every packet, and briefly pulses the LED amber a few
  times if the sender flags its battery as low.
- **espnow_receiver_c3_buzzer/espnow_receiver_c3_buzzer.ino** — an
  alternate, buzzer-only receiver for an ESP32-C3 devboard (no
  NeoPixel/LED dependency). Plays a 5-beep, 800 Hz, 200 ms on/200 ms
  off alarm pattern on a passive buzzer when a packet arrives, and
  silences/clears it when its own BOOT button is pressed. This is a
  separate sketch from receiver/receiver.ino - use whichever matches
  your hardware.

## How it avoids needing MAC address pairing

ESP-NOW normally requires you to register the peer's MAC address before
sending. To keep setup simple, the sender uses the ESP-NOW **broadcast**
address (`FF:FF:FF:FF:FF:FF`), so it doesn't need to know the receiver's
MAC in advance, and any receiver on the same Wi-Fi channel will pick up
the packet. Both sketches default to channel `1` (the default when
neither board joins a Wi-Fi network). If you later connect either board
to a Wi-Fi router, set `ESPNOW_CHANNEL` on both sketches to match the
router's channel.

## Battery monitoring

The sender reports its battery voltage/percentage with every button
press, in the same packet as the button signal (no extra wake-ups).

- **Hardware needed**: the ESP32-S3-Zero has no built-in fuel gauge, so
  you must wire an external 2-resistor voltage divider from
  battery+ to `BATTERY_ADC_PIN` (default GPIO1) to GND. Size the
  resistors so the divided voltage stays within the ADC's ~0-3.3V
  range at the battery's maximum voltage — e.g. two equal resistors
  (such as 100kΩ + 100kΩ) give a 2:1 divider, safe for a single-cell
  LiPo topping out around 4.2V. Update `BATTERY_DIVIDER_RATIO` in
  `sender.ino` to match your resistor values:
  `ratio = (R1 + R2) / R2` (R1 battery-side, R2 GND-side).
- **Thresholds**: `BATTERY_EMPTY_MILLIVOLTS`/`BATTERY_FULL_MILLIVOLTS`
  set the voltage range mapped to 0-100%, and `BATTERY_LOW_PERCENT`
  sets when the `lowBattery` flag is set (default 20%). Adjust these
  for your specific battery chemistry/capacity.
- **Does this cost significant extra battery charge?** No — reading the
  ADC a handful of times only takes on the order of microseconds and
  happens while the Wi-Fi radio is already powered on to transmit the
  ESP-NOW packet (which is the dominant current draw during that brief
  active window, typically tens of milliamps for well under a second).
  The added ADC sampling time is negligible next to that, and utterly
  negligible next to the deep-sleep baseline current, which is what
  actually determines battery life between button presses.

## Troubleshooting: repeated "E BOD: Brownout detector was triggered"

If the receiver's Serial Monitor shows a boot loop like:

```
rst:0x3 (RTC_SW_SYS_RST), boot:0x8 (SPI_FAST_FLASH_BOOT)
...
E BOD: Brownout detector was triggered
```

repeating over and over, the chip's 3.3V rail is briefly dipping below
its brownout threshold (~2.43V) — almost always because the USB
cable/port/hub can't supply the current spike the chip draws while
switching to full (240MHz) clock speed and/or initializing the Wi-Fi
radio at boot. The reset then re-triggers the same spike, looping
forever.

`receiver.ino` writes `RTC_CNTL_BROWN_OUT_REG` at the start of
`setup()` and lowers the radio's max TX power as a best-effort
mitigation, **but this does not always work on ESP32-S3**: that reset
frequently happens during the ROM bootloader / early clock-init stage,
*before* `setup()` is even reached — at which point nothing in the
sketch can prevent it, since the register write happens too late.
**If the loop persists after reflashing with this mitigation in place
(as confirmed in testing), that is expected and confirms the crash is
happening pre-`setup()`.** There is no way to fix this from an Arduino
.ino sketch; it requires either a hardware fix or a custom
ESP-IDF/sdkconfig build (out of scope for the Arduino IDE workflow used
here). To resolve it:
- **Try lowering the CPU frequency** via Arduino IDE's
  `Tools > CPU Frequency` menu (e.g. from 240MHz down to 160MHz or
  80MHz) before re-uploading. This reduces the current spike at the
  clock-switch that often triggers the brownout, and is worth trying
  first since it requires no hardware changes.
- Use a shorter/thicker, known-good USB data+power cable, plugged
  directly into a USB port (not a hub or extension cable) that can
  supply enough current — a PC's own USB 3.0 port or a dedicated USB
  power adapter (not just a keyboard/monitor USB passthrough port).
- Add a decoupling/bulk capacitor (e.g. 470-1000uF electrolytic) across
  the board's 5V (or 3V3) and GND pins, close to the board, to buffer
  the current spike.
- If brownouts persist even after these, the board itself or its
  onboard regulator may be marginal/faulty (this is a known/reported
  issue on some ESP32-S3 boards, independent of what firmware is
  flashed).

## Setup

1. Open `sender/sender.ino` in Arduino IDE, select your ESP32S3 Zero
   board (e.g. "ESP32S3 Dev Module" with USB CDC enabled), wire the
   battery voltage divider (see above), and upload.
2. Open `receiver/receiver.ino`, select your ESP32S3 devboard, install
   the "Adafruit NeoPixel" library via Library Manager if not already
   installed, and upload.
3. Power both boards over USB. Open the Serial Monitor for each
   (115200 baud) to watch logs, including battery voltage/percentage.
4. Press the BOOT button on the sender — the receiver's onboard LED
   should turn red and stay lit; it also logs the battery reading.
5. Press the BOOT button on the receiver to clear the LED.

## Things you may need to adjust

- **Onboard LED**: the receiver targets the **HW-678** ESP32-S3 N8R2
  devboard, whose onboard LED is an addressable WS2812 ("NeoPixel")
  RGB LED on GPIO48, driven via the Adafruit_NeoPixel library
  (install it from Arduino IDE's Library Manager). It lights red when
  latched. If you swap in a different devboard with a plain GPIO LED
  instead, replace the NeoPixel calls in `receiver.ino` with a simple
  `digitalWrite`. For an ESP32-C3 board, use
  `espnow_receiver_c3_buzzer/espnow_receiver_c3_buzzer.ino` instead,
  which drives a buzzer and has no LED/NeoPixel dependency at all.
- **Buzzer pin (C3 variant)**: `espnow_receiver_c3_buzzer.ino` defaults
  `BUZZER_PIN` to GPIO4 and `BOOT_BUTTON_GPIO` to GPIO9 - both are
  placeholders and should be updated at the top of the file to match
  your specific ESP32-C3 board.
- **BOOT button pin**: both sketches assume GPIO0, standard for ESP32-S3
  boards. Confirm this matches your specific board's schematic.
- **Deep sleep current**: for best battery life on the sender, also
  disable unused peripherals/brownout detector as needed for your
  specific board revision.

## Open questions

If any of the following differ from the assumptions above, let me know
and the sketches can be adjusted:

- Whether you want a range/reliability improvement (e.g. resending a
  few times, or an ACK from receiver back to sender) instead of a
  single fire-and-forget broadcast.
- Whether the sender should also flash its own LED briefly to confirm
  a press was registered before sleeping.

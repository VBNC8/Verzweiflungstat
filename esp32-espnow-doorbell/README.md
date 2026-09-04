# ESP-NOW Deep-Sleep Pushbutton Signal (ESP32S3 Zero -> ESP32S3 DevBoard)

Arduino IDE sketches for a battery-powered wireless "call button":

- **sender/sender.ino** — runs on the ESP32S3 Zero. Sleeps in deep sleep to
  save battery. Pressing the BOOT button (GPIO0) wakes it, it sends one
  ESP-NOW broadcast packet, waits for the button release, then goes
  straight back to deep sleep.
- **receiver/receiver.ino** — runs on the ESP32S3 devboard (HW-678),
  powered continuously over USB. Turns the onboard WS2812 RGB LED on
  when it receives a packet, and turns it off only when its own BOOT
  button is pressed.

## How it avoids needing MAC address pairing

ESP-NOW normally requires you to register the peer's MAC address before
sending. To keep setup simple, the sender uses the ESP-NOW **broadcast**
address (`FF:FF:FF:FF:FF:FF`), so it doesn't need to know the receiver's
MAC in advance, and any receiver on the same Wi-Fi channel will pick up
the packet. Both sketches default to channel `1` (the default when
neither board joins a Wi-Fi network). If you later connect either board
to a Wi-Fi router, set `ESPNOW_CHANNEL` on both sketches to match the
router's channel.

## Setup

1. Open `sender/sender.ino` in Arduino IDE, select your ESP32S3 Zero
   board (e.g. "ESP32S3 Dev Module" with USB CDC enabled), and upload.
2. Open `receiver/receiver.ino`, select your ESP32S3 devboard, install
   the "Adafruit NeoPixel" library via Library Manager if not already
   installed, and upload.
3. Power both boards over USB. Open the Serial Monitor for each
   (115200 baud) to watch logs.
4. Press the BOOT button on the sender — the receiver's onboard LED
   should turn on and stay on.
5. Press the BOOT button on the receiver to clear the LED.

## Things you may need to adjust

- **Onboard LED**: the receiver targets the **HW-678** ESP32-S3 N8R2
  devboard, whose onboard LED is an addressable WS2812 ("NeoPixel")
  RGB LED on GPIO48, driven via the Adafruit_NeoPixel library
  (install it from Arduino IDE's Library Manager). It lights red when
  latched. If you swap in a different devboard with a plain GPIO LED
  instead, replace the NeoPixel calls in `receiver.ino` with a simple
  `digitalWrite`.
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

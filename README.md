LED Clock cube


Clock displays hours in red (top)
Upper three red 3mm LEDs indicate 6h each
Lower five red 3mm LEDs indicate 1h each
0h to 23h can be displayed

Clock displays minutes in green (bottom)
Upper nine green 3mm LEDs indicate 1min each
Lower five green 3mm LEDs indicate 10min each
0min to 59min can be displayed

Clock displays seconds by alternating middle two orange LEDs

Hour and minute LEDs fade in an out to minimize surprise effect by abrupt brightness change
Seconds do not fade

Clock has two modes: Battery mode and USB mode

In USB mode time is displayed constantly
One tap (G-sensor) enters date display mode. For 7sec left orange Sec LED is lit constantly and the current date is displayed by using red h-LEDs as month and green min-LEDs as day

After 7sec it jumps back to time display unless the clock is tapped (G-sensor) in date loop. If it detects a tap in date look, the OTA feature is activated with 1min timeout
OTA mode is shown by green W fading in and out

In Battery mode the ESP is in deep sleep mode
Time display can be activated by tapping the clock
Time is displayed for 7sec, after that the date is displayed for 7sec
After that, the ESP goes to sleep again to save battery

During Battery mode the clock displayes the time echa 15min for 10sec and goes to sleep again after that
If the time is a little off, the rounded time is displayed instead, so only :00, :15, :30 and :45

Clock gets time/date via WLAN

Upon startup/restart the clock lights both sec LES with fadein/-out to show activity

Light sensor increases and decreases LED brightness automatically

Clock uses 5x5 matrix for all 24 LEDs to save GPIOs
Clock uses INT1 from G-Sensor and USB+ from charger module to wakeup the ESP during deep sleep, both protected by diodes

Firmware for Hawk handwired hotswap wireless 5x3+3 split mechanical ergo keyboard with two SuperMini nrf52840 controllers.  
Left Alps RKJXT1F42001 5-way encoder switch.  
Left 'mechanical' BT-selector switch.
3D printed upper case half.
3D printed case bottom plate.
500mAh battery on each half mounted below the case bottom plate.
Moulded tin tenting weights under each half, total kb weight 1100g for both halves.

## Keymap
![Keymap](./keymap-drawer/hawk.svg)  

<img width="1253" height="1053" alt="20260508_212808" src="https://github.com/user-attachments/assets/f1f7a164-3dd1-45e8-ab4b-03623d4e0525" />

<img width="1286" height="1051" alt="p-hbhes-01" src="https://github.com/user-attachments/assets/c471b0eb-48ba-4e91-9c07-c3e09c3d9000" />

<img width="1510" height="1078" alt="20260515_140231" src="https://github.com/user-attachments/assets/e20ec464-2853-446a-95fb-59018f012847" />
Basis for the layout was a testrig with five adjustable finger triplets and a thumb triplet for MX switches and keycaps. Once I was satisfied with the adjustment, I took the geometry as basis for the Hawk.

<img width="1078" height="1152" alt="20260515_173813" src="https://github.com/user-attachments/assets/218abfc9-5443-4e62-9552-8aff25addeb9" />
BT Selector internals.
Springs are 4mm diameter taken from MX blue switches out of an old Cherry G80, Pushrods for the TS09-63-25-WT-260-SMT-TR tactile SMD switches on top of the narrow PCB pressed in the pushbuttons and located inside the springs are cut to length bicycle spokes, double diodes on the bottom of the PCB are BAV70 type.

<img width="1151" height="593" alt="Gemini_Generated_Image_5l3mhj5l3mhj5l3m" src="https://github.com/user-attachments/assets/b2d541b1-e478-434a-b4aa-65ba6d2fc74b" />
One contact of each selector SMD switch is wired to GND and the other to TWO keys each with BAV70 double diodes to save space. One of the keys is common to all four bushbuttons (Q) and the others are differnt obviously (Y, X, C and V). In ZMK they are processed as combos.


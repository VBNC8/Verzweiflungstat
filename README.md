Firmware for Hawk handwired hotswap wireless 5x3+3 split mechanical ergo keyboard with two SuperMini nRF52840 controllers.  
Left Alps RKJXT1F42001 5-way encoder multiswitch.  
Left 'mechanical' BT-selector switch.
3D printed upper case half.
3D printed case bottom plate.
500mAh battery on each half mounted below the case bottom plate.
Moulded tin tenting weights under each half, total kb weight 1100g for both halves.

## Keymap
![Keymap](./keymap-drawer/hawk.svg)  

<img width="1124" height="944" alt="Left-half" src="https://github.com/user-attachments/assets/891a8795-94cd-42e0-9496-7664ea36a665" />

<img width="1286" height="1051" alt="p-hbhes-01" src="https://github.com/user-attachments/assets/c471b0eb-48ba-4e91-9c07-c3e09c3d9000" />

<img width="1510" height="1078" alt="20260515_140231" src="https://github.com/user-attachments/assets/e20ec464-2853-446a-95fb-59018f012847" />
Basis for the layout was a testrig with five adjustable finger triplets and a thumb triplet for MX switches and keycaps. Once I was satisfied with the adjustment, I took the geometry as basis for the Hawk.

<img width="2611" height="1015" alt="Selector" src="https://github.com/user-attachments/assets/46d42be8-2cda-4e5c-9ce1-3bf2deb4138b" />
BT Selector internals.
Springs are 4mm diameter taken from MX blue switches out of an old Cherry G80, Pushrods for the TS09-63-25-WT-260-SMT-TR tactile SMD switches on top of the narrow PCB pressed in the pushbuttons and located inside the springs are cut to length bicycle spokes, double diodes on the bottom of the PCB are BAV70 type.

<img width="2168" height="899" alt="Diagram1" src="https://github.com/user-attachments/assets/135bc677-fd78-4d62-b7ad-a3a43dce3c93" />
One contact of each selector SMD switch is wired to GND and the other to TWO keys each with BAV70 double diodes to save space. One of the keys is common to all four bushbuttons (X) and the others are differnt obviously (A, S, D and G). In ZMK they are processed as combos.

The Alps multiswitch is processed in a similar manner.

<img width="1234" height="1078" alt="Alps-mount" src="https://github.com/user-attachments/assets/9f9116e4-fd44-472d-b46e-40374c0bed90" />
To mount the Alps multiswitch rigidly to the PLA top plate, a small PCB with four mounting holes is used.

<img width="1406" height="1002" alt="Hawk-PCB-hardware" src="https://github.com/user-attachments/assets/2e36497b-da44-4c97-ad06-579c5d9ca554" />
Both Selector PCB (4 and 5 button type) and Alps mount PCB (round and rectangular type w/ matrix or direct-pin option) are produced on a less than 100x100mm PCB as one piece. [Gerber-file](./gerber/Gerber_Y4.zip)

<img width="1408" height="654" alt="Tin" src="https://github.com/user-attachments/assets/264480dc-02cf-48f5-b65c-f9c54817c228" />
For the tin tenting weights, first a PLA positive was printed for each side with space for batteries and BT-Selector, plaster/sand moulds formed and filled with molten tin. Final drill holes, some local machining and polishing was necessary to make the weights ready for assembly.

<img width="1670" height="1028" alt="20260508_232749" src="https://github.com/user-attachments/assets/4c0a40eb-d154-4367-aa4e-ad4a43c904fc" />
As there is enough space below the board in the tenting wedge, the board is powered by two 500mAh 3.7V 503035 Lipo Polymer Batteries.

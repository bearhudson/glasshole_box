### Based on:

* Brilliant Labs (frame-codebase & monocle-micropython)

- The 128-bit Nordic Semi UART Service UUID (6e400001-b5a3-f393-e0a9-e50e24dcca9e), which Brilliant Labs uses as the primary BLE transport protocol for both the Frame and Monocle open-source glasses.

* OpenGlass / Based Hardware (OpenGlass & Friend/OMI)

* Advertised name signatures and standard Nordic UART / ESP32-S3 BLE GATT advertisement profiles used in DIY open-source AI smart glasses and wearable hardware.

* Reverse-Engineered Ray-Ban Meta Protocols (Community Wireshark / BLE Captures & Sniffers)

* The Luxottica Bluetooth SIG Company Identifier (0x0D53).

* The standard Meta 16-bit Service UUID (0xFD5F) broadcasted during discovery/setup states.

* The proprietary vendor payload prefix (META_RB) present in secondary advertising packets.

* Even Realities Community BLE Analysis (G1 protocol reverse-engineering)

* The base custom GATT service UUID (00002760-08c2-11e1-9073-0e8ac72e0001) that the Even Realities G1 glasses use for left/right temple coordination and mobile app pairing.

* ESP32 BLE Library & Wardriving / Sniffer Tools (ESP32-BLE-Collector / ESP32-BLE-Scanner)

- The asynchronous callback structure (BLEAdvertisedDeviceCallbacks), active scanning logic (setActiveScan(true) to pull scan-response packets), and low-level RSSI filtering design.

* Heltec Automation ESP32 Display & Power Management Examples (Heltec-Example / Meshtastic ESP32-S3 BSP)

- The hardware-specific pin mappings for the Heltec V3 (Vext on GPIO 36, I2C display routing on GPIO 17/18/21, and PRG interrupt-driven button handling on GPIO 0).

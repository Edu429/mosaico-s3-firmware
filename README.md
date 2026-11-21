# Mosaico S3

A lightweight Mosaico-inspired firmware for ESP32-S3 boards with 16 MB flash and 8 MB octal PSRAM.

This project is not the official ESP-Mosaico firmware: the official Mosaico platform uses ESP32-S31 hardware and
ESP-IDF 6.2+. It provides the basic boot experience and remote management features that can run on an
ESP32-S3-N16R8 board without a display.

## Features

- Mosaico-style responsive web boot dashboard
- Serial boot information
- Wi-Fi provisioning through an access point
- Wi-Fi credentials stored in NVS
- System status API
- OTA firmware upload
- Remote restart
- 16 MB flash and 8 MB PSRAM configuration

## ESP-IDF build

This repository now includes native ESP-IDF support alongside the original PlatformIO prototype.

In an ESP-IDF 5.x or 6.x terminal:

```powershell
cd C:\Users\edwin\.cline\data\workspaces\chat\mosaico-s3-firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

## First boot

1. Install PlatformIO.
2. Open this folder in VS Code.
3. Connect the ESP32-S3-N16R8 by USB.
4. Run `PlatformIO: Upload`.
5. Run `PlatformIO: Serial Monitor`.

On first boot, the board starts an access point named `Mosaico-XXXXXX` with password `mosaico123`.
Open `http://192.168.4.1`, enter your Wi-Fi credentials, and wait for the board to restart.

## APIs

- `GET /` - Web boot dashboard
- `GET /api/status` - Device and network status
- `POST /api/wifi` - Save Wi-Fi credentials
- `POST /api/update` - Upload a firmware `.bin`
- `POST /api/restart` - Restart the board

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C3 firmware project for reading LiTime LiFePO4 battery data via BLE (Bluetooth Low Energy) and publishing metrics to MQTT. Built with PlatformIO and Arduino framework.

## Build Commands

```bash
# Build the project
pio run

# Upload to device
pio run --target upload

# Serial monitor (115200 baud)
pio device monitor

# Build and upload in one step
pio run --target upload && pio device monitor

# Clean build artifacts
pio run --target clean
```

## Architecture

- **Target**: ESP32-C3 SuperMini (esp32-c3-devkitm-1 board)
- **Framework**: Arduino
- **Platform**: Espressif32
- **USB**: CDC on boot enabled for serial communication

### Directory Structure

- `src/` - Main application code (entry point: main.cpp)
- `lib/` - Project-specific libraries
- `include/` - Header files
- `test/` - Unit tests (run with `pio test`)
- `platformio.ini` - Build configuration and dependencies

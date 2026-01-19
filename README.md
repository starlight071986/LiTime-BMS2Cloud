# LiTime LiFePO4 BMS Monitor

ESP32-C3 Firmware zur Überwachung von LiTime LiFePO4-Batterien via Bluetooth Low Energy (BLE) mit Webinterface und Home Assistant Integration.

Basiert auf der BLE-Bibliothek [Litime_BMS_ESP32](https://github.com/mirosieber/Litime_BMS_ESP32) von [@mirosieber](https://github.com/mirosieber).

## Funktionen

- **BLE-Verbindung** zum LiTime BMS zur Abfrage aller Batterie-Parameter
- **Webinterface** zur Anzeige aller BMS-Daten mit automatischer Aktualisierung
- **Home Assistant Integration** via Webhook (JSON-Datenübertragung)
- **Access Point Modus** zur Erstkonfiguration ohne bestehende WLAN-Infrastruktur
- **mDNS-Unterstützung** - erreichbar unter `http://litime-bms.local`
- **NTP-Zeitsynchronisation** mit konfigurierbarer Zeitzone
- **Robuste Fehlerbehandlung** bei Verbindungsabbrüchen (WLAN, BLE, Internet)
- **LED-Statusanzeige** - Interne LED zeigt Verbindungsstatus durch Blinkmuster
- **WLAN-Sendestärke** - Konfigurierbar im Webinterface (Niedrig/Normal/Hoch)
- **Signalqualität-Anzeige** - RSSI in dBm mit menschenlesbarer Bewertung

## Hardware

- **Mikrocontroller**: ESP32-C3 SuperMini
- **Batterie**: LiTime LiFePO4 mit BLE-fähigem BMS

## Installation

### Voraussetzungen

- [PlatformIO](https://platformio.org/) (CLI oder VS Code Extension)
- USB-Kabel für ESP32-C3

### Build & Upload

```bash
# Projekt kompilieren
pio run

# Auf Gerät flashen
pio run --target upload

# Seriellen Monitor öffnen (115200 Baud)
pio device monitor

# Kompilieren, flashen und Monitor in einem Schritt
pio run --target upload && pio device monitor
```

## Ersteinrichtung

1. **ESP32-C3 flashen** und mit Strom versorgen
2. **WLAN-Netzwerk** `LiTime-BMS-XXXX` suchen (XXXX = Teil der MAC-Adresse)
3. **Verbinden** mit Passwort `12345678`
4. **Browser öffnen**: `http://192.168.4.1`
5. Auf der **WLAN-Seite** das Heimnetzwerk konfigurieren
6. Auf der **Bluetooth-Seite** die BMS MAC-Adresse eintragen

## Webinterface

### Seiten

| Seite | Beschreibung |
|-------|--------------|
| **Werte** | BMS-Übersicht, detaillierte Werte, Zellspannungen |
| **Bluetooth** | BMS-Verbindung, MAC-Adresse, Abfrageintervall |
| **Cloud** | Home Assistant Webhook-Konfiguration |
| **WLAN** | Netzwerkeinstellungen, Sendestärke, Signalqualität, Zeitzone |

### Statusleiste

Die Statusleiste oben zeigt den aktuellen Zustand aller Verbindungen:

| Status | Grün | Gelb | Rot | Grau |
|--------|------|------|-----|------|
| WLAN | Verbunden | AP-Modus | Getrennt | - |
| Internet | Verfügbar | - | Nicht verfügbar | - |
| NTP | Synchronisiert | - | Nicht synchronisiert | - |
| Terminal | Aktiviert | - | - | Deaktiviert |
| Bluetooth | Aktiviert | - | - | Deaktiviert |
| BMS | Verbunden | - | Getrennt | - |
| Cloud | OK | - | Fehler | Deaktiviert |

### LED-Statusanzeige

Die interne LED (GPIO 8) des ESP32-C3 SuperMini zeigt den Verbindungsstatus durch Blinkmuster an. Die Anzahl der Blinker zeigt an, welche Verbindung fehlt:

| Blinkmuster | Bedeutung |
|-------------|-----------|
| **1x Blinken** | WLAN nicht verbunden |
| **2x Blinken** | BMS nicht verbunden oder keine gültigen Daten |
| **3x Blinken** | Cloud-Fehler beim Senden |
| **Dauerhaft an** | Alle aktivierten Dienste funktionieren |

**Soll-Zustand wird berücksichtigt:**
- Bluetooth deaktiviert → BMS-Status wird nicht geprüft
- Cloud deaktiviert → Cloud-Status wird nicht geprüft

Beispiel: Bei deaktiviertem Bluetooth und deaktivierter Cloud leuchtet die LED dauerhaft sobald WLAN verbunden ist.

## Home Assistant Integration

### Webhook einrichten

1. In Home Assistant: **Einstellungen** → **Automatisierungen** → **Neue Automatisierung**
2. **Trigger**: Webhook
3. Webhook-ID notieren (z.B. `litime_bms_data`)
4. Die vollständige URL im Webinterface unter **Cloud** eintragen:
   ```
   http://homeassistant.local:8123/api/webhook/litime_bms_data
   ```

### JSON-Datenstruktur

```json
{
  "device": "litime-bms",
  "mac": "XX:XX:XX:XX:XX:XX",
  "timestamp": "19.01.2026 14:30:00",
  "connected": true,
  "battery": {
    "voltage": 26.4,
    "current": -2.5,
    "soc": 85,
    "soh": "100%",
    "remaining_ah": 85.0,
    "full_capacity_ah": 100.0
  },
  "temperature": {
    "mosfet": 25,
    "cells": 23
  },
  "status": {
    "battery_state": "Discharging",
    "protection_state": "Normal",
    "failure_state": "Normal",
    "heat_state": "Off"
  },
  "cell_voltages": [3.30, 3.30, 3.30, 3.30, 3.30, 3.30, 3.30, 3.30],
  "statistics": {
    "discharge_cycles": 42,
    "discharged_ah": 4200.5
  }
}
```

## Konfiguration

### Einstellungen

| Parameter | Standard | Beschreibung |
|-----------|----------|--------------|
| BMS MAC | - | MAC-Adresse des BMS (Format: XX:XX:XX:XX:XX:XX) |
| Abfrageintervall | 20s | Intervall für BMS-Datenabfrage (5-300s) |
| WLAN Sendestärke | Niedrig | Sendeleistung: Niedrig (5 dBm), Normal (11 dBm), Hoch (17 dBm) |
| Zeitzone | Berlin | POSIX-Zeitzonenformat |
| Webhook URL | - | Home Assistant Webhook-URL |
| Webhook Intervall | 60s | Sendeintervall für Webhook (10-3600s) |

### WLAN Sendestärke

Die Sendeleistung kann im Webinterface unter **WLAN** angepasst werden:

| Stufe | Leistung | Verwendung |
|-------|----------|------------|
| **Niedrig** | 5 dBm | Empfohlen - minimale Wärmeentwicklung |
| **Normal** | 11 dBm | Bei Reichweitenproblemen - moderate Wärme |
| **Hoch** | 17 dBm | Maximale Reichweite - erhöhte Wärmeentwicklung |

⚠️ **Hinweis:** Höhere Sendeleistung führt zu mehr Wärmeentwicklung. Bei dauerhaftem Betrieb wird "Niedrig" empfohlen.

### Signalqualität

Die aktuelle WLAN-Empfangsstärke wird auf der WLAN-Seite angezeigt:

| RSSI | Bewertung | Farbe |
|------|-----------|-------|
| ≥ -50 dBm | Hervorragend | Grün |
| -51 bis -60 dBm | Sehr gut | Grün |
| -61 bis -70 dBm | Gut | Gelb |
| -71 bis -80 dBm | Ausreichend | Orange |
| < -80 dBm | Schwach | Rot |

### Datenvalidierung

BMS-Daten werden nur verwendet, wenn sie plausibel sind:
- Gesamtspannung: 10V - 60V
- SOC: 0% - 100%
- Zellspannungen: 2.0V - 4.0V

## Technische Details

### Architektur

```
┌─────────────────────────────────────────────────────────────┐
│                     ESP32-C3 SuperMini                      │
├─────────────────────────────────────────────────────────────┤
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────────────┐ │
│  │  WiFi   │  │   BLE   │  │   NTP   │  │   Webserver     │ │
│  │ Station │  │ Client  │  │  Sync   │  │   (Port 80)     │ │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────────┬────────┘ │
│       │            │            │                 │          │
│       v            v            v                 v          │
│  ┌─────────────────────────────────────────────────────────┐│
│  │                    Hauptschleife                        ││
│  │  - WLAN Reconnect (non-blocking)                        ││
│  │  - BMS Datenabfrage                                     ││
│  │  - Webhook-Versand                                      ││
│  │  - NTP-Synchronisation                                  ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
         │                    │
         v                    v
    ┌─────────┐         ┌──────────┐
    │  LiTime │         │   Home   │
    │   BMS   │         │Assistant │
    └─────────┘         └──────────┘
```

### Speicherpartitionierung

Das Projekt verwendet `huge_app.csv` für 3MB App-Speicher, um den kombinierten BLE+WiFi-Stack unterzubringen.

### Energieverbrauch

Die WLAN-Sendeleistung ist standardmäßig auf "Niedrig" (5 dBm) eingestellt, um Wärmeentwicklung zu minimieren. Bei Reichweitenproblemen kann die Sendestärke im Webinterface unter **WLAN** erhöht werden.

## Fehlerbehebung

### WLAN-Verbindung instabil

- Signalqualität auf der WLAN-Seite prüfen (sollte mindestens "Ausreichend" sein)
- WLAN-Sendeleistung im Webinterface erhöhen
- Nähe zum Router prüfen

### BMS wird nicht gefunden

- MAC-Adresse überprüfen (Format: XX:XX:XX:XX:XX:XX)
- Entfernung zum BMS verringern (BLE-Reichweite ca. 10m)
- BMS einschalten bzw. Batterie aktivieren

### Webinterface nicht erreichbar

- IP-Adresse im seriellen Monitor prüfen
- `http://litime-bms.local` nur im gleichen Netzwerk verfügbar
- Im AP-Modus: `http://192.168.4.1`

## Abhängigkeiten

- [Litime_BMS_ESP32](https://github.com/mirosieber/Litime_BMS_ESP32) - BLE-Kommunikation mit LiTime BMS
- [ArduinoJson](https://arduinojson.org/) - JSON-Serialisierung

## Lizenz

MIT License

## Mitwirken

Pull Requests und Issues sind willkommen!

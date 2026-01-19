/**
 * ============================================================================
 * LiTime LiFePO4 BMS Monitor - ESP32-C3 Firmware
 * ============================================================================
 *
 * Diese Firmware ermöglicht die Überwachung von LiTime LiFePO4-Batterien
 * über Bluetooth Low Energy (BLE) mit einem modernen Webinterface und
 * optionaler Home Assistant Integration via Webhook.
 *
 * Hauptfunktionen:
 * - BLE-Verbindung zum LiTime BMS zur Abfrage aller Batterie-Parameter
 * - Responsives Webinterface mit Echtzeit-Aktualisierung (1 Sekunde)
 * - Home Assistant Integration via HTTP Webhook (JSON-Format)
 * - Access Point Modus zur Erstkonfiguration ohne WLAN-Infrastruktur
 * - mDNS-Unterstützung (erreichbar unter http://litime-bms.local)
 * - NTP-Zeitsynchronisation mit konfigurierbarer Zeitzone
 * - Robuste Fehlerbehandlung bei Verbindungsabbrüchen
 *
 * Hardware: ESP32-C3 SuperMini
 * Framework: Arduino
 *
 * Autor: Marcel Mayer
 * Lizenz: MIT
 * ============================================================================
 */

// ============================================================================
// Bibliotheken einbinden
// ============================================================================
#include <Arduino.h>          // Arduino-Kernfunktionen für ESP32
#include <WiFi.h>             // WLAN-Funktionen (Station und Access Point)
#include <WebServer.h>        // HTTP-Webserver für das Webinterface
#include <ESPmDNS.h>          // mDNS für lokale Namensauflösung (litime-bms.local)
#include <BMSClient.h>        // BLE-Client für LiTime BMS Kommunikation
#include <time.h>             // Zeitfunktionen für NTP-Synchronisation
#include <Preferences.h>      // Persistenter Speicher (NVS) für Einstellungen
#include <ArduinoJson.h>      // JSON-Serialisierung für API und Webhook
#include <nvs_flash.h>        // Non-Volatile Storage Flash-Initialisierung
#include <HTTPClient.h>       // HTTP-Client für Webhook-Anfragen

// ============================================================================
// Konstanten und Konfiguration
// ============================================================================

// WLAN Access Point Konfiguration
#define AP_PASSWORD "12345678"      // Passwort für den Access Point Modus
#define WIFI_TIMEOUT_MS 30000       // Timeout für WLAN-Verbindungsversuche (30 Sekunden)

// ============================================================================
// Globale Objekte
// ============================================================================

// BMS Bluetooth Client - kommuniziert via BLE mit dem LiTime BMS
BMSClient bmsClient;

// MAC-Adresse des BMS - muss vom Benutzer im Webinterface konfiguriert werden
// Format: XX:XX:XX:XX:XX:XX (17 Zeichen)
String bmsMac = "";

// HTTP-Webserver auf Port 80 für das Webinterface
WebServer server(80);

// Preferences-Objekt für persistente Speicherung im NVS (Non-Volatile Storage)
// Überlebt Neustarts und Stromausfälle
Preferences preferences;

// ============================================================================
// WLAN Status-Variablen
// ============================================================================

// MAC-Adresse des ESP32 (wird beim Start ausgelesen)
String macAddress;

// SSID für den Access Point Modus (z.B. "LiTime-BMS-AB12")
String apSSID;

// Aktueller WLAN-Modus: true = Access Point, false = Station (mit Router verbunden)
bool apMode = false;

// Flag für laufenden WLAN-Reconnect-Versuch (non-blocking)
bool wifiReconnecting = false;

// Zeitstempel wann der Reconnect-Versuch gestartet wurde
unsigned long wifiReconnectStart = 0;

// ============================================================================
// Benutzer-Einstellungen (werden persistent gespeichert)
// ============================================================================

// Zeitzone im POSIX-Format für korrekte Sommerzeit-Umstellung
// Standard: Berlin (CET-1CEST,M3.5.0,M10.5.0/3)
String timezone = "CET-1CEST,M3.5.0,M10.5.0/3";

// Intervall für BMS-Datenabfrage in Sekunden (5-300)
unsigned long bmsInterval = 20;

// Bluetooth aktiviert/deaktiviert
bool bluetoothEnabled = true;

// BMS-Verbindungsstatus
bool bmsConnected = false;

// Terminal-Ausgabe der BMS-Daten aktiviert/deaktiviert
bool serialOutputEnabled = true;

// Flag ob die aktuellen BMS-Daten plausibel sind
// Wird bei jedem Update neu berechnet
bool bmsDataValid = false;

// Flag für ausstehende BMS-Verbindung (non-blocking Verbindungsaufbau)
bool bmsConnectPending = false;

// ============================================================================
// Home Assistant Webhook-Konfiguration
// ============================================================================

// Webhook-URL für Home Assistant (z.B. http://homeassistant.local:8123/api/webhook/xxx)
String haWebhookUrl = "";

// Sendeintervall für Webhook in Sekunden (10-3600)
unsigned long haInterval = 60;

// Webhook aktiviert/deaktiviert
bool haEnabled = false;

// Zeitstempel des letzten Webhook-Versands
unsigned long lastHaSend = 0;

// Zeitpunkt des letzten Webhook-Versands als lesbarer String
String lastHaTime = "";

// HTTP-Statuscode der letzten Webhook-Anfrage
int lastHaHttpCode = 0;

// Response-Text der letzten Webhook-Anfrage (für Debugging)
String lastHaResponse = "";

// ============================================================================
// Timing-Variablen für non-blocking Operationen
// ============================================================================
// Alle zeitgesteuerten Operationen verwenden millis() statt delay()
// um den Webserver nicht zu blockieren

// Zeitstempel der letzten BMS-Datenabfrage
unsigned long lastBmsUpdate = 0;

// Zeitstempel der letzten NTP-Synchronisation
unsigned long lastNtpSync = 0;

// Intervall für NTP-Synchronisation (1 Stunde in Millisekunden)
unsigned long ntpSyncInterval = 3600000;

// Unix-Timestamp der letzten erfolgreichen NTP-Synchronisation
time_t lastSyncTime = 0;

// Zeitstempel der letzten WLAN-Verbindungsprüfung
unsigned long lastWifiCheck = 0;

// Intervall für WLAN-Verbindungsprüfung (30 Sekunden)
unsigned long wifiCheckInterval = 30000;

// NTP-Server für Zeitsynchronisation
const char* ntpServer = "pool.ntp.org";

// ============================================================================
// BMS-Datenstruktur
// ============================================================================
// Speichert alle vom BMS abgefragten Werte zwischen den Abfragen
// Wird im Webinterface und für Webhook-Daten verwendet

struct BMSData {
  float totalVoltage = 0;           // Gesamtspannung der Batterie in Volt
  float cellVoltageSum = 0;         // Summe aller Zellspannungen in Volt
  float current = 0;                // Aktueller Strom in Ampere (negativ = Entladen)
  int16_t mosfetTemp = 0;           // MOSFET-Temperatur in °C
  int16_t cellTemp = 0;             // Zellentemperatur in °C
  uint8_t soc = 0;                  // State of Charge (Ladezustand) in Prozent
  String soh = "";                  // State of Health (Batteriegesundheit)
  float remainingAh = 0;            // Verbleibende Kapazität in Ah
  float fullCapacityAh = 0;         // Volle Kapazität in Ah
  String protectionState = "";      // Schutzstatus (z.B. "Normal", "Overvoltage")
  String heatState = "";            // Heizungsstatus
  String balanceMemory = "";        // Balance-Speicher
  String failureState = "";         // Fehlerstatus
  String balancingState = "";       // Balancing-Status
  String batteryState = "";         // Batteriestatus (Charging/Discharging/Idle)
  uint32_t dischargesCount = 0;     // Anzahl der Entladezyklen
  float dischargesAhCount = 0;      // Gesamte entladene Ah über Lebensdauer
  std::vector<float> cellVoltages;  // Einzelne Zellspannungen als Vektor
} bmsData;

// ============================================================================
// Forward-Deklarationen
// ============================================================================
// Funktionen die vor ihrer Definition aufgerufen werden

void printBMSDataSerial();    // Gibt BMS-Daten auf Serial aus
void startAP();               // Startet den Access Point Modus
bool connectToSavedWiFi();    // Verbindet mit gespeichertem WLAN

// ============================================================================
// Einstellungen speichern und laden
// ============================================================================

/**
 * Speichert alle Benutzereinstellungen im NVS (Non-Volatile Storage)
 *
 * Diese Funktion wird aufgerufen wenn der Benutzer Einstellungen
 * im Webinterface ändert. Die Daten überleben Neustarts.
 */
void saveSettings() {
  // NVS-Namespace "settings" im Schreibmodus öffnen
  preferences.begin("settings", false);

  // Alle Einstellungen speichern
  preferences.putString("timezone", timezone);
  preferences.putULong("bmsInterval", bmsInterval);
  preferences.putBool("btEnabled", bluetoothEnabled);
  preferences.putString("bmsMac", bmsMac);
  preferences.putString("haWebhook", haWebhookUrl);
  preferences.putULong("haInterval", haInterval);
  preferences.putBool("haEnabled", haEnabled);
  preferences.putBool("serialOut", serialOutputEnabled);

  // Namespace schließen um Änderungen zu persistieren
  preferences.end();
}

/**
 * Lädt alle Benutzereinstellungen aus dem NVS
 *
 * Wird beim Start aufgerufen. Falls keine Einstellungen vorhanden sind,
 * werden die Standardwerte (zweiter Parameter) verwendet.
 */
void loadSettings() {
  // NVS-Namespace "settings" im Lesemodus öffnen
  preferences.begin("settings", true);

  // Einstellungen laden mit Standardwerten als Fallback
  timezone = preferences.getString("timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
  bmsInterval = preferences.getULong("bmsInterval", 20);
  bluetoothEnabled = preferences.getBool("btEnabled", true);
  bmsMac = preferences.getString("bmsMac", "");
  haWebhookUrl = preferences.getString("haWebhook", "");
  haInterval = preferences.getULong("haInterval", 60);
  haEnabled = preferences.getBool("haEnabled", false);
  serialOutputEnabled = preferences.getBool("serialOut", true);

  preferences.end();
}

// ============================================================================
// BMS-Datenvalidierung
// ============================================================================

/**
 * Prüft ob die aktuellen BMS-Daten plausibel sind
 *
 * Diese Funktion verhindert die Anzeige/Übertragung von ungültigen Daten,
 * die z.B. direkt nach dem Verbindungsaufbau auftreten können (alle Werte 0).
 *
 * Prüfkriterien für LiFePO4-Batterien:
 * - Gesamtspannung: 10V - 60V
 * - SOC: 0% - 100%
 * - Zellspannungen: 2.0V - 4.0V (etwas großzügiger für Randfälle)
 *
 * @return true wenn alle Daten plausibel sind, sonst false
 */
bool isBmsDataValid() {
  // Spannung muss zwischen 10V und 60V liegen (typisch für LiFePO4 4S-16S)
  if (bmsData.totalVoltage < 10.0 || bmsData.totalVoltage > 60.0) {
    return false;
  }

  // SOC muss zwischen 0 und 100 Prozent sein
  if (bmsData.soc > 100) {
    return false;
  }

  // Mindestens eine Zellspannung muss vorhanden sein
  if (bmsData.cellVoltages.size() == 0) {
    return false;
  }

  // Alle Zellspannungen müssen plausibel sein
  // LiFePO4: Nominal 3.2V, Bereich ca. 2.5V - 3.65V
  // Wir verwenden 2.0V - 4.0V für etwas Toleranz
  for (float v : bmsData.cellVoltages) {
    if (v < 2.0 || v > 4.0) {
      return false;
    }
  }

  return true;
}

// ============================================================================
// Zeit-Funktionen
// ============================================================================

/**
 * Synchronisiert die Systemzeit mit einem NTP-Server
 *
 * Verwendet die konfigurierte Zeitzone für automatische
 * Sommer-/Winterzeit-Umstellung.
 */
void syncNTP() {
  // Zeitzone und NTP-Server konfigurieren
  configTzTime(timezone.c_str(), ntpServer);

  // Warten auf Synchronisation (max. 10 Sekunden)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    // Zeitstempel der erfolgreichen Synchronisation speichern
    lastSyncTime = time(nullptr);
    Serial.println("NTP synchronisiert");
  }
}

/**
 * Gibt die aktuelle Zeit als formatierten String zurück
 *
 * @return Zeit im Format "DD.MM.YYYY HH:MM:SS" oder Fehlermeldung
 */
String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Zeit nicht verfügbar";
  }
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

/**
 * Gibt den Zeitpunkt der letzten NTP-Synchronisation zurück
 *
 * @return Zeitpunkt als String oder "Noch nicht synchronisiert"
 */
String getLastSyncTimeString() {
  if (lastSyncTime == 0) {
    return "Noch nicht synchronisiert";
  }
  struct tm* timeinfo = localtime(&lastSyncTime);
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo);
  return String(buffer);
}

// ============================================================================
// BMS-Datenabfrage
// ============================================================================

/**
 * Fragt alle Daten vom BMS ab und speichert sie in der globalen Struktur
 *
 * Diese Funktion:
 * 1. Prüft ob eine BMS-Verbindung besteht
 * 2. Ruft update() am BMS-Client auf um neue Daten zu holen
 * 3. Kopiert alle Werte in die lokale Datenstruktur
 * 4. Validiert die Daten auf Plausibilität
 * 5. Gibt die Daten optional auf Serial aus
 */
void updateBMSData() {
  // Abbrechen wenn keine Verbindung besteht
  if (!bmsConnected) return;

  // BMS-Client auffordern neue Daten zu holen
  bmsClient.update();

  // Alle Werte in die lokale Struktur kopieren
  bmsData.totalVoltage = bmsClient.getTotalVoltage();
  bmsData.cellVoltageSum = bmsClient.getCellVoltageSum();
  bmsData.current = bmsClient.getCurrent();
  bmsData.mosfetTemp = bmsClient.getMosfetTemp();
  bmsData.cellTemp = bmsClient.getCellTemp();
  bmsData.soc = bmsClient.getSOC();
  bmsData.soh = bmsClient.getSOH();
  bmsData.remainingAh = bmsClient.getRemainingAh();
  bmsData.fullCapacityAh = bmsClient.getFullCapacityAh();
  bmsData.protectionState = bmsClient.getProtectionState();
  bmsData.heatState = bmsClient.getHeatState();
  bmsData.balanceMemory = bmsClient.getBalanceMemory();
  bmsData.failureState = bmsClient.getFailureState();
  bmsData.balancingState = bmsClient.getBalancingState();
  bmsData.batteryState = bmsClient.getBatteryState();
  bmsData.dischargesCount = bmsClient.getDischargesCount();
  bmsData.dischargesAhCount = bmsClient.getDischargesAhCount();
  bmsData.cellVoltages = bmsClient.getCellVoltages();

  // Plausibilitätsprüfung durchführen
  bool wasValid = bmsDataValid;
  bmsDataValid = isBmsDataValid();

  // Bei ungültigen Daten: Meldung ausgeben und abbrechen
  if (!bmsDataValid) {
    if (wasValid) {
      Serial.println("[BMS] Daten nicht plausibel - überspringe Ausgabe/Webhook");
    }
    return;  // Keine weitere Verarbeitung bei ungültigen Daten
  }

  // Wenn Daten wieder plausibel werden: Meldung ausgeben
  if (!wasValid && bmsDataValid) {
    Serial.println("[BMS] Daten jetzt plausibel - Ausgabe aktiviert");
  }

  // Optional: Daten auf Serial ausgeben
  if (serialOutputEnabled) {
    printBMSDataSerial();
  }
}

/**
 * Gibt eine kompakte Übersicht der BMS-Daten auf Serial aus
 *
 * Wird nur aufgerufen wenn serialOutputEnabled true ist.
 */
void printBMSDataSerial() {
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("                   LiTime BMS Status                   ");
  Serial.println("══════════════════════════════════════════════════════");
  Serial.printf("Gesamtspannung: %.2f V | SOC: %d%% | Strom: %.2f A\n",
    bmsData.totalVoltage, bmsData.soc, bmsData.current);
  Serial.printf("Temperatur: MOSFET %d°C | Zellen %d°C\n",
    bmsData.mosfetTemp, bmsData.cellTemp);
  Serial.println();
}

// ============================================================================
// HTML-Templates für das Webinterface
// ============================================================================
// Die Templates verwenden Raw String Literals (R"rawliteral(...)rawliteral")
// um HTML/CSS/JavaScript direkt einzubetten ohne Escape-Zeichen

/**
 * HTML-Header mit CSS-Styles und Navigation
 *
 * Das Design verwendet ein dunkles Farbschema mit Akzentfarbe #4ecca3 (Türkis)
 * und ist vollständig responsiv für mobile Geräte.
 */
const char* HTML_HEADER = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>LiTime BMS Monitor</title>
  <style>
    /* Reset und Basis-Styles */
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #1a1a2e; color: #eee; min-height: 100vh; }

    /* Navigation */
    .nav { background: #16213e; padding: 1rem; display: flex; gap: 1rem; flex-wrap: wrap; }
    .nav a { color: #4ecca3; text-decoration: none; padding: 0.5rem 1rem; border-radius: 5px; transition: background 0.3s; }
    .nav a:hover, .nav a.active { background: #4ecca3; color: #1a1a2e; }

    /* Container und Karten */
    .container { max-width: 900px; margin: 0 auto; padding: 1rem; }
    .card { background: #16213e; border-radius: 10px; padding: 1.5rem; margin-bottom: 1rem; }
    .card h2 { color: #4ecca3; margin-bottom: 1rem; border-bottom: 1px solid #4ecca3; padding-bottom: 0.5rem; }

    /* Grid-Layout für Statistiken */
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; }
    .stat { background: #1a1a2e; padding: 1rem; border-radius: 8px; text-align: center; }
    .stat-value { font-size: 1.8rem; font-weight: bold; color: #4ecca3; }
    .stat-label { font-size: 0.9rem; color: #888; margin-top: 0.3rem; }

    /* Formular-Elemente */
    input, select { width: 100%; padding: 0.8rem; margin: 0.5rem 0; border: 1px solid #4ecca3; border-radius: 5px; background: #1a1a2e; color: #eee; }
    button { background: #4ecca3; color: #1a1a2e; padding: 0.8rem 1.5rem; border: none; border-radius: 5px; cursor: pointer; font-weight: bold; margin-top: 0.5rem; }
    button:hover { background: #3db892; }

    /* Toggle-Switch (iOS-Style) */
    .toggle { display: flex; align-items: center; gap: 1rem; }
    .toggle-switch { position: relative; width: 60px; height: 30px; }
    .toggle-switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #ccc; border-radius: 30px; transition: 0.4s; }
    .slider:before { position: absolute; content: ""; height: 22px; width: 22px; left: 4px; bottom: 4px; background: white; border-radius: 50%; transition: 0.4s; }
    input:checked + .slider { background: #4ecca3; }
    input:checked + .slider:before { transform: translateX(30px); }

    /* Status-Badges */
    .status { padding: 0.3rem 0.8rem; border-radius: 15px; font-size: 0.85rem; }
    .status.connected { background: #4ecca3; color: #1a1a2e; }
    .status.disconnected { background: #e74c3c; color: white; }

    /* Zeitanzeige */
    .time-display { font-size: 2rem; font-weight: bold; color: #4ecca3; text-align: center; padding: 1rem; }

    /* Tabellen */
    table { width: 100%; border-collapse: collapse; }
    td { padding: 0.5rem; border-bottom: 1px solid #333; }
    td:first-child { color: #888; }
    td:last-child { text-align: right; color: #4ecca3; }

    /* Zellspannungs-Grid */
    .cell-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(80px, 1fr)); gap: 0.5rem; }
    .cell { background: #1a1a2e; padding: 0.5rem; border-radius: 5px; text-align: center; font-size: 0.85rem; }
    .cell-num { color: #888; font-size: 0.75rem; }

    /* Statusleiste oben */
    .status-bar { background: #0d1117; padding: 0.5rem 1rem; display: flex; gap: 0.8rem; flex-wrap: wrap; font-size: 0.75rem; border-bottom: 1px solid #333; }
    .status-item { display: flex; align-items: center; gap: 0.3rem; }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; }
    .status-dot.green { background: #4ecca3; }
    .status-dot.red { background: #e74c3c; }
    .status-dot.yellow { background: #f39c12; }
    .status-dot.gray { background: #666; }

    /* Nicht verfügbare Bereiche */
    .unavailable { opacity: 0.5; }
    .unavailable-msg { text-align: center; padding: 2rem; color: #888; }
  </style>
</head>
<body>
  <!-- Hauptnavigation -->
  <nav class="nav">
    <a href="/" id="nav-values">Werte</a>
    <a href="/bluetooth" id="nav-bluetooth">Bluetooth</a>
    <a href="/cloud" id="nav-cloud">Cloud</a>
    <a href="/wlan" id="nav-wlan">WLAN</a>
  </nav>
  <!-- Statusleiste wird per JavaScript befüllt -->
  <div class="status-bar" id="statusBar"></div>
  <div class="container">
)rawliteral";

/**
 * HTML-Footer mit JavaScript für aktive Navigation
 */
const char* HTML_FOOTER = R"rawliteral(
  </div>
  <script>
    // Aktiven Navigationspunkt hervorheben
    const path = window.location.pathname;
    document.querySelectorAll('.nav a').forEach(a => {
      if (a.getAttribute('href') === path || (path === '/' && a.id === 'nav-values')) {
        a.classList.add('active');
      }
    });
  </script>
</body>
</html>
)rawliteral";

// ============================================================================
// Webseiten-Handler
// ============================================================================

/**
 * Handler für die Hauptseite (Werte-Übersicht)
 *
 * Zeigt:
 * - Aktuelle Uhrzeit mit NTP-Sync-Status
 * - BMS-Übersicht (SOC, Spannung, Strom, Temperatur)
 * - Detaillierte Werte (alle BMS-Parameter)
 * - Zellspannungen als Grid
 *
 * Bei fehlender BMS-Verbindung werden die Bereiche ausgegraut
 * und eine entsprechende Meldung angezeigt.
 */
void handleRoot() {
  String html = HTML_HEADER;

  // Prüfen ob BMS-Daten verfügbar sind (Bluetooth aktiv UND verbunden UND Daten plausibel)
  bool bmsAvailable = bluetoothEnabled && bmsConnected && bmsDataValid;

  // Zeit-Karte (immer sichtbar)
  html += R"rawliteral(
    <div class="card">
      <h2>Zeit</h2>
      <div class="time-display" id="currentTime">--:--:--</div>
      <table>
        <tr><td>Letzte NTP Synchronisierung</td><td id="lastSync">)rawliteral";
  html += getLastSyncTimeString();
  html += R"rawliteral(</td></tr>
      </table>
    </div>

    <div class="card)rawliteral";
  // CSS-Klasse "unavailable" hinzufügen wenn BMS nicht verfügbar
  if (!bmsAvailable) html += " unavailable";
  html += R"rawliteral(" id="bmsOverview">
      <h2>BMS Übersicht</h2>)rawliteral";

  // Bei nicht verfügbarem BMS: Fehlermeldung anzeigen
  if (!bmsAvailable) {
    html += R"rawliteral(
      <div class="unavailable-msg">)rawliteral";
    // Spezifische Fehlermeldung je nach Ursache
    if (!bluetoothEnabled) {
      html += "Bluetooth ist deaktiviert";
    } else if (!bmsConnected) {
      html += "Keine Verbindung zum BMS";
    } else {
      html += "BMS-Daten nicht verfügbar";
    }
    html += R"rawliteral(</div>)rawliteral";
  } else {
    // BMS-Übersicht mit den vier wichtigsten Werten
    html += R"rawliteral(
      <div class="grid">
        <div class="stat">
          <div class="stat-value" id="soc">)rawliteral";
    html += String(bmsData.soc);
    html += R"rawliteral(%</div>
          <div class="stat-label">Ladezustand</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="voltage">)rawliteral";
    html += String(bmsData.totalVoltage, 2);
    html += R"rawliteral( V</div>
          <div class="stat-label">Spannung</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="current">)rawliteral";
    html += String(bmsData.current, 2);
    html += R"rawliteral( A</div>
          <div class="stat-label">Strom</div>
        </div>
        <div class="stat">
          <div class="stat-value" id="temp">)rawliteral";
    html += String(bmsData.cellTemp);
    html += R"rawliteral( °C</div>
          <div class="stat-label">Temperatur</div>
        </div>
      </div>)rawliteral";
  }
  html += R"rawliteral(
    </div>

    <div class="card)rawliteral";
  if (!bmsAvailable) html += " unavailable";
  html += R"rawliteral(" id="bmsDetails">
      <h2>Detaillierte Werte</h2>)rawliteral";

  if (!bmsAvailable) {
    html += R"rawliteral(
      <div class="unavailable-msg">)rawliteral";
    if (!bluetoothEnabled) {
      html += "Bluetooth ist deaktiviert";
    } else if (!bmsConnected) {
      html += "Keine Verbindung zum BMS";
    } else {
      html += "BMS-Daten nicht verfügbar";
    }
    html += R"rawliteral(</div>)rawliteral";
  } else {
    // Detaillierte Wertetabelle
    html += R"rawliteral(
      <table>
        <tr><td>Gesamtspannung</td><td id="totalVoltage">)rawliteral";
    html += String(bmsData.totalVoltage, 2) + " V</td></tr>";
    html += "<tr><td>Zellspannungssumme</td><td id=\"cellVoltageSum\">" + String(bmsData.cellVoltageSum, 2) + " V</td></tr>";
    html += "<tr><td>Strom</td><td id=\"currentDetail\">" + String(bmsData.current, 2) + " A</td></tr>";
    html += "<tr><td>SOC</td><td id=\"socDetail\">" + String(bmsData.soc) + " %</td></tr>";
    html += "<tr><td>SOH</td><td id=\"soh\">" + bmsData.soh + "</td></tr>";
    html += "<tr><td>Verbleibende Kapazität</td><td id=\"remainingAh\">" + String(bmsData.remainingAh, 2) + " Ah</td></tr>";
    html += "<tr><td>Volle Kapazität</td><td id=\"fullCapacity\">" + String(bmsData.fullCapacityAh, 2) + " Ah</td></tr>";
    html += "<tr><td>MOSFET Temperatur</td><td id=\"mosfetTemp\">" + String(bmsData.mosfetTemp) + " °C</td></tr>";
    html += "<tr><td>Zellen Temperatur</td><td id=\"cellTempDetail\">" + String(bmsData.cellTemp) + " °C</td></tr>";
    html += "<tr><td>Batteriestatus</td><td id=\"batteryState\">" + bmsData.batteryState + "</td></tr>";
    html += "<tr><td>Schutzstatus</td><td id=\"protectionState\">" + bmsData.protectionState + "</td></tr>";
    html += "<tr><td>Fehlerstatus</td><td id=\"failureState\">" + bmsData.failureState + "</td></tr>";
    html += "<tr><td>Heizung</td><td id=\"heatState\">" + bmsData.heatState + "</td></tr>";
    html += "<tr><td>Entladezyklen</td><td id=\"discharges\">" + String(bmsData.dischargesCount) + "</td></tr>";
    html += "<tr><td>Entladene Ah</td><td id=\"dischargesAh\">" + String(bmsData.dischargesAhCount, 2) + " Ah</td></tr>";
    html += R"rawliteral(
      </table>)rawliteral";
  }
  html += R"rawliteral(
    </div>

    <div class="card)rawliteral";
  if (!bmsAvailable) html += " unavailable";
  html += R"rawliteral(" id="bmsCells">
      <h2>Zellspannungen</h2>)rawliteral";

  if (!bmsAvailable) {
    html += R"rawliteral(
      <div class="unavailable-msg">)rawliteral";
    if (!bluetoothEnabled) {
      html += "Bluetooth ist deaktiviert";
    } else if (!bmsConnected) {
      html += "Keine Verbindung zum BMS";
    } else {
      html += "BMS-Daten nicht verfügbar";
    }
    html += R"rawliteral(</div>)rawliteral";
  } else {
    // Zellspannungen als Grid
    html += R"rawliteral(
      <div class="cell-grid" id="cellGrid">)rawliteral";
    for (size_t i = 0; i < bmsData.cellVoltages.size(); i++) {
      html += "<div class=\"cell\"><div class=\"cell-num\">Zelle " + String(i + 1) + "</div>";
      html += String(bmsData.cellVoltages[i], 3) + " V</div>";
    }
    html += R"rawliteral(
      </div>)rawliteral";
  }
  html += R"rawliteral(
    </div>

    <script>
      /**
       * Aktualisiert die Statusleiste mit aktuellen Verbindungsstatus
       * Wird alle 2 Sekunden aufgerufen
       */
      function updateStatusBar() {
        fetch('/api/status').then(r => r.json()).then(s => {
          let html = '';
          // WLAN-Status: grün=verbunden, gelb=AP-Modus, rot=getrennt
          html += '<div class="status-item"><div class="status-dot ' + (s.wlanConnected ? 'green' : (s.apMode ? 'yellow' : 'red')) + '"></div>WLAN: ' + (s.apMode ? 'AP' : (s.wlanConnected ? 'OK' : 'Aus')) + '</div>';
          // Internet: grün wenn NTP erfolgreich war
          html += '<div class="status-item"><div class="status-dot ' + (s.internetOk ? 'green' : 'red') + '"></div>Internet</div>';
          // NTP: grün wenn synchronisiert
          html += '<div class="status-item"><div class="status-dot ' + (s.ntpSynced ? 'green' : 'red') + '"></div>NTP</div>';
          // Terminal: grün=aktiviert, grau=deaktiviert
          html += '<div class="status-item"><div class="status-dot ' + (s.serialEnabled ? 'green' : 'gray') + '"></div>Terminal</div>';
          // Bluetooth: grün=aktiviert, grau=deaktiviert
          html += '<div class="status-item"><div class="status-dot ' + (s.btEnabled ? 'green' : 'gray') + '"></div>Bluetooth</div>';
          // BMS: grün=verbunden, rot=getrennt
          html += '<div class="status-item"><div class="status-dot ' + (s.bmsConnected ? 'green' : 'red') + '"></div>BMS</div>';
          // Cloud: grün=OK, rot=Fehler, grau=deaktiviert
          html += '<div class="status-item"><div class="status-dot ' + (s.cloudEnabled ? (s.cloudOk ? 'green' : 'red') : 'gray') + '"></div>Cloud</div>';
          document.getElementById('statusBar').innerHTML = html;
        });
      }

      /**
       * Aktualisiert die Zeitanzeige
       * Wird jede Sekunde aufgerufen
       */
      function updateTime() {
        fetch('/api/time').then(r => r.json()).then(data => {
          document.getElementById('currentTime').textContent = data.time;
        });
      }

      /**
       * Aktualisiert alle BMS-Daten auf der Seite
       * Wird jede Sekunde aufgerufen
       */
      function updateData() {
        fetch('/api/data').then(r => r.json()).then(data => {
          // Prüfen ob BMS-Daten verfügbar sind
          if (!data.available) {
            // Bereiche ausgrauen wenn nicht verfügbar
            ['bmsOverview', 'bmsDetails', 'bmsCells'].forEach(id => {
              document.getElementById(id).classList.add('unavailable');
            });
            return;
          }
          // Bereiche wieder aktivieren
          ['bmsOverview', 'bmsDetails', 'bmsCells'].forEach(id => {
            document.getElementById(id).classList.remove('unavailable');
          });
          // Alle Werte aktualisieren (nur wenn Elemente existieren)
          if (document.getElementById('soc')) {
            document.getElementById('soc').textContent = data.soc + '%';
            document.getElementById('voltage').textContent = data.totalVoltage.toFixed(2) + ' V';
            document.getElementById('current').textContent = data.current.toFixed(2) + ' A';
            document.getElementById('temp').textContent = data.cellTemp + ' °C';
            document.getElementById('totalVoltage').textContent = data.totalVoltage.toFixed(2) + ' V';
            document.getElementById('cellVoltageSum').textContent = data.cellVoltageSum.toFixed(2) + ' V';
            document.getElementById('currentDetail').textContent = data.current.toFixed(2) + ' A';
            document.getElementById('socDetail').textContent = data.soc + ' %';
            document.getElementById('soh').textContent = data.soh;
            document.getElementById('remainingAh').textContent = data.remainingAh.toFixed(2) + ' Ah';
            document.getElementById('fullCapacity').textContent = data.fullCapacityAh.toFixed(2) + ' Ah';
            document.getElementById('mosfetTemp').textContent = data.mosfetTemp + ' °C';
            document.getElementById('cellTempDetail').textContent = data.cellTemp + ' °C';
            document.getElementById('batteryState').textContent = data.batteryState;
            document.getElementById('protectionState').textContent = data.protectionState;
            document.getElementById('failureState').textContent = data.failureState;
            document.getElementById('heatState').textContent = data.heatState;
            document.getElementById('discharges').textContent = data.dischargesCount;
            document.getElementById('dischargesAh').textContent = data.dischargesAhCount.toFixed(2) + ' Ah';

            // Zellspannungen dynamisch neu rendern
            let cellHtml = '';
            data.cellVoltages.forEach((v, i) => {
              cellHtml += '<div class="cell"><div class="cell-num">Zelle ' + (i+1) + '</div>' + v.toFixed(3) + ' V</div>';
            });
            document.getElementById('cellGrid').innerHTML = cellHtml;
          }
        });
      }

      // Update-Intervalle starten
      setInterval(updateStatusBar, 2000);  // Statusleiste alle 2 Sekunden
      setInterval(updateTime, 1000);        // Zeit jede Sekunde
      setInterval(updateData, 1000);        // BMS-Daten jede Sekunde

      // Sofort beim Laden aktualisieren
      updateStatusBar();
      updateTime();
      updateData();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

/**
 * Handler für die Bluetooth-Einstellungsseite
 *
 * Ermöglicht:
 * - Bluetooth ein-/ausschalten
 * - Terminal-Ausgabe ein-/ausschalten
 * - BMS MAC-Adresse konfigurieren
 * - Abfrageintervall einstellen
 */
void handleBluetooth() {
  String html = HTML_HEADER;

  html += R"rawliteral(
    <div class="card">
      <h2>Bluetooth Verbindung</h2>
      <div style="display: flex; align-items: center; gap: 1rem; margin-bottom: 1rem;">
        <span>Status:</span>
        <span class="status )rawliteral";
  // Verbindungsstatus als Badge
  html += bmsConnected ? "connected\">Verbunden" : "disconnected\">Getrennt";
  html += R"rawliteral(</span>
      </div>
      <!-- Toggle für Bluetooth -->
      <div class="toggle">
        <span>Bluetooth aktivieren</span>
        <label class="toggle-switch">
          <input type="checkbox" id="btToggle" )rawliteral";
  html += bluetoothEnabled ? "checked" : "";
  html += R"rawliteral( onchange="toggleBluetooth(this.checked)">
          <span class="slider"></span>
        </label>
      </div>
      <!-- Toggle für Terminal-Ausgabe -->
      <div class="toggle" style="margin-top:1rem;">
        <span>Terminal-Ausgabe</span>
        <label class="toggle-switch">
          <input type="checkbox" id="serialToggle" )rawliteral";
  html += serialOutputEnabled ? "checked" : "";
  html += R"rawliteral( onchange="toggleSerial(this.checked)">
          <span class="slider"></span>
        </label>
      </div>
    </div>

    <div class="card">
      <h2>Einstellungen</h2>
      <label>BMS MAC-Adresse</label>
      <input type="text" id="bmsMac" value=")rawliteral";
  html += bmsMac;
  html += R"rawliteral(" placeholder="XX:XX:XX:XX:XX:XX" style="font-family: monospace;">
      <label>Abfrageintervall (Sekunden)</label>
      <input type="number" id="interval" value=")rawliteral";
  html += String(bmsInterval);
  html += R"rawliteral(" min="5" max="300">
      <button onclick="saveSettings()">Speichern</button>
    </div>

    <script>
      // Terminal-Ausgabe umschalten
      function toggleSerial(enabled) {
        fetch('/api/serial', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled})
        });
      }

      // Bluetooth umschalten
      function toggleBluetooth(enabled) {
        fetch('/api/bluetooth', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled})
        }).then(() => location.reload());
      }

      // Einstellungen speichern (MAC und Intervall)
      function saveSettings() {
        const mac = document.getElementById('bmsMac').value;
        const interval = document.getElementById('interval').value;
        fetch('/api/bms-settings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({mac: mac, interval: parseInt(interval)})
        }).then(() => {
          alert('Gespeichert! Gerät startet neu...');
          setTimeout(() => location.reload(), 3000);
        });
      }
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

// ============================================================================
// Home Assistant Webhook
// ============================================================================

/**
 * Sendet alle BMS-Daten als JSON an den Home Assistant Webhook
 *
 * Prüft vor dem Senden:
 * - Ob Webhook aktiviert ist
 * - Ob eine URL konfiguriert ist
 * - Ob WLAN verbunden ist
 * - Ob BMS-Daten plausibel sind
 *
 * @return true wenn erfolgreich gesendet, sonst false
 */
bool sendToHomeAssistant() {
  // Prüfen ob Webhook überhaupt senden soll
  if (!haEnabled || haWebhookUrl.length() == 0 || apMode) {
    return false;
  }

  // WLAN-Verbindung prüfen
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HA] Kein WLAN - überspringe Webhook");
    lastHaTime = getCurrentTimeString();
    lastHaHttpCode = -1;
    lastHaResponse = "Kein WLAN";
    return false;
  }

  // Daten-Plausibilität prüfen
  if (!bmsDataValid) {
    Serial.println("[HA] BMS-Daten nicht plausibel - überspringe Webhook");
    lastHaTime = getCurrentTimeString();
    lastHaHttpCode = -1;
    lastHaResponse = "BMS-Daten nicht plausibel";
    return false;
  }

  // HTTP-Client konfigurieren mit Timeouts
  HTTPClient http;
  http.setTimeout(10000);       // Gesamttimeout: 10 Sekunden
  http.setConnectTimeout(5000); // Verbindungsaufbau: 5 Sekunden
  http.begin(haWebhookUrl);
  http.addHeader("Content-Type", "application/json");

  // JSON-Dokument erstellen
  JsonDocument doc;
  doc["device"] = "litime-bms";
  doc["mac"] = macAddress;
  doc["timestamp"] = getCurrentTimeString();
  doc["connected"] = bmsConnected;

  // Batterie-Daten als Unterobjekt
  JsonObject battery = doc["battery"].to<JsonObject>();
  battery["voltage"] = bmsData.totalVoltage;
  battery["current"] = bmsData.current;
  battery["soc"] = bmsData.soc;
  battery["soh"] = bmsData.soh;
  battery["remaining_ah"] = bmsData.remainingAh;
  battery["full_capacity_ah"] = bmsData.fullCapacityAh;

  // Temperaturen als Unterobjekt
  JsonObject temps = doc["temperature"].to<JsonObject>();
  temps["mosfet"] = bmsData.mosfetTemp;
  temps["cells"] = bmsData.cellTemp;

  // Status-Informationen als Unterobjekt
  JsonObject status = doc["status"].to<JsonObject>();
  status["battery_state"] = bmsData.batteryState;
  status["protection_state"] = bmsData.protectionState;
  status["failure_state"] = bmsData.failureState;
  status["heat_state"] = bmsData.heatState;

  // Zellspannungen als Array
  JsonArray cells = doc["cell_voltages"].to<JsonArray>();
  for (float v : bmsData.cellVoltages) {
    cells.add(v);
  }

  // Statistiken als Unterobjekt
  JsonObject stats = doc["statistics"].to<JsonObject>();
  stats["discharge_cycles"] = bmsData.dischargesCount;
  stats["discharged_ah"] = bmsData.dischargesAhCount;

  // JSON serialisieren und senden
  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  // Status für Anzeige im Webinterface speichern
  lastHaTime = getCurrentTimeString();
  lastHaHttpCode = httpCode;

  if (httpCode > 0) {
    // Erfolgreiche Antwort (auch Fehler wie 404 etc.)
    lastHaResponse = http.getString();
    // Response auf 200 Zeichen kürzen für Anzeige
    if (lastHaResponse.length() > 200) {
      lastHaResponse = lastHaResponse.substring(0, 200) + "...";
    }
  } else {
    // Negative Codes sind Verbindungsfehler
    switch (httpCode) {
      case -1: lastHaResponse = "Verbindung fehlgeschlagen"; break;
      case -2: lastHaResponse = "Senden fehlgeschlagen"; break;
      case -3: lastHaResponse = "Kein Stream"; break;
      case -4: lastHaResponse = "Keine HTTP-Verbindung"; break;
      case -5: lastHaResponse = "Verbindung verloren"; break;
      case -11: lastHaResponse = "Timeout"; break;
      default: lastHaResponse = "Fehler " + String(httpCode);
    }
  }

  http.end();

  // Erfolg loggen
  if (httpCode == 200) {
    Serial.println("[HA] Daten erfolgreich gesendet");
    return true;
  } else {
    Serial.printf("[HA] Fehler: HTTP %d - %s\n", httpCode, lastHaResponse.c_str());
    return false;
  }
}

/**
 * Handler für die Cloud-Einstellungsseite (Home Assistant)
 *
 * Ermöglicht:
 * - Webhook aktivieren/deaktivieren
 * - Webhook-URL konfigurieren
 * - Sendeintervall einstellen
 * - Test-Webhook senden
 * - Letzten Webhook-Status anzeigen
 * - JSON-Vorschau anzeigen
 */
void handleCloud() {
  String html = HTML_HEADER;

  html += R"rawliteral(
    <div class="card">
      <h2>Home Assistant</h2>
      <p style="color:#888;margin-bottom:1rem;">Sendet BMS-Daten per Webhook an Home Assistant.</p>

      <div class="toggle" style="margin-bottom:1rem;">
        <span>Webhook aktivieren</span>
        <label class="toggle-switch">
          <input type="checkbox" id="haEnabled" )rawliteral";
  html += haEnabled ? "checked" : "";
  html += R"rawliteral( onchange="toggleHA(this.checked)">
          <span class="slider"></span>
        </label>
      </div>

      <label>Webhook URL</label>
      <input type="text" id="haWebhook" value=")rawliteral";
  html += haWebhookUrl;
  html += R"rawliteral(" placeholder="http://homeassistant.local:8123/api/webhook/WEBHOOK_ID">

      <label>Sendeintervall (Sekunden)</label>
      <input type="number" id="haInterval" value=")rawliteral";
  html += String(haInterval);
  html += R"rawliteral(" min="10" max="3600">

      <button onclick="saveHA()">Speichern</button>
      <button onclick="testHA()" style="background:#666;margin-left:0.5rem;">Jetzt senden</button>
    </div>

    <div class="card">
      <h2>Letzter Webhook</h2>
      <table>
        <tr><td>Zeitpunkt</td><td id="lastTime">)rawliteral";
  html += lastHaTime.length() > 0 ? lastHaTime : "Noch nicht gesendet";
  html += R"rawliteral(</td></tr>
        <tr><td>HTTP Status</td><td id="lastCode">)rawliteral";
  // HTTP-Statuscode als farbiges Badge
  if (lastHaHttpCode > 0) {
    html += "<span class=\"status " + String(lastHaHttpCode == 200 ? "connected" : "disconnected") + "\">" + String(lastHaHttpCode) + "</span>";
  } else {
    html += "-";
  }
  html += R"rawliteral(</td></tr>
        <tr><td>Response</td><td id="lastResponse" style="word-break:break-all;">)rawliteral";
  html += lastHaResponse.length() > 0 ? lastHaResponse : "-";
  html += R"rawliteral(</td></tr>
      </table>
    </div>

    <div class="card">
      <h2>JSON Vorschau</h2>
      <p style="color:#888;margin-bottom:0.5rem;">Diese Daten werden an Home Assistant gesendet:</p>
      <pre id="jsonPreview" style="background:#1a1a2e;padding:1rem;border-radius:8px;overflow-x:auto;font-size:0.8rem;color:#4ecca3;"></pre>
      <button onclick="refreshPreview()">Aktualisieren</button>
    </div>

    <script>
      // Webhook aktivieren/deaktivieren
      function toggleHA(enabled) {
        fetch('/api/ha-settings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled, url: document.getElementById('haWebhook').value, interval: parseInt(document.getElementById('haInterval').value)})
        }).then(() => location.reload());
      }

      // Webhook-Einstellungen speichern
      function saveHA() {
        const url = document.getElementById('haWebhook').value;
        const interval = document.getElementById('haInterval').value;
        const enabled = document.getElementById('haEnabled').checked;
        fetch('/api/ha-settings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled, url: url, interval: parseInt(interval)})
        }).then(() => {
          alert('Gespeichert!');
        });
      }

      // Test-Webhook senden
      function testHA() {
        fetch('/api/ha-test', {method: 'POST'})
          .then(r => r.json())
          .then(d => {
            location.reload();
          });
      }

      // JSON-Vorschau aktualisieren
      function refreshPreview() {
        fetch('/api/data').then(r => r.json()).then(data => {
          const preview = {
            device: "litime-bms",
            mac: ")rawliteral";
  html += macAddress;
  html += R"rawliteral(",
            timestamp: new Date().toLocaleString('de-DE'),
            connected: data.connected,
            battery: {
              voltage: data.totalVoltage,
              current: data.current,
              soc: data.soc,
              soh: data.soh,
              remaining_ah: data.remainingAh,
              full_capacity_ah: data.fullCapacityAh
            },
            temperature: {
              mosfet: data.mosfetTemp,
              cells: data.cellTemp
            },
            status: {
              battery_state: data.batteryState,
              protection_state: data.protectionState,
              failure_state: data.failureState,
              heat_state: data.heatState
            },
            cell_voltages: data.cellVoltages,
            statistics: {
              discharge_cycles: data.dischargesCount,
              discharged_ah: data.dischargesAhCount
            }
          };
          document.getElementById('jsonPreview').textContent = JSON.stringify(preview, null, 2);
        });
      }

      // Vorschau beim Laden aktualisieren
      refreshPreview();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

/**
 * Handler für die WLAN-Einstellungsseite
 *
 * Ermöglicht:
 * - WLAN-Status anzeigen (AP-Modus oder verbunden)
 * - Verfügbare Netzwerke scannen
 * - Mit neuem Netzwerk verbinden
 * - WLAN zurücksetzen (AP-Modus starten)
 * - Zeitzone konfigurieren
 */
void handleWlan() {
  String html = HTML_HEADER;

  html += R"rawliteral(
    <div class="card">
      <h2>WLAN Status</h2>
      <div id="status"></div>
    </div>

    <div class="card" id="networkCard">
      <h2>Netzwerk wechseln</h2>
      <button onclick="scanNetworks()">Netzwerke suchen</button>
      <div id="networks" style="margin-top: 1rem;"></div>
    </div>

    <div class="card" id="resetCard" style="display:none;">
      <h2>WLAN zurücksetzen</h2>
      <p style="color:#888;margin-bottom:1rem;">Löscht die gespeicherten WLAN-Daten und startet den Access Point Modus.</p>
      <button style="background:#e74c3c;" onclick="resetWiFi()">WLAN zurücksetzen</button>
    </div>

    <div class="card">
      <h2>Zeitzone</h2>
      <label>Zeitzone (POSIX Format)</label>
      <select id="timezone" onchange="document.getElementById('tzCustom').style.display = this.value === 'custom' ? 'block' : 'none'">
        <option value="CET-1CEST,M3.5.0,M10.5.0/3" )rawliteral";
  html += timezone == "CET-1CEST,M3.5.0,M10.5.0/3" ? "selected" : "";
  html += R"rawliteral(>Berlin (CET/CEST)</option>
        <option value="GMT0BST,M3.5.0/1,M10.5.0" )rawliteral";
  html += timezone == "GMT0BST,M3.5.0/1,M10.5.0" ? "selected" : "";
  html += R"rawliteral(>London (GMT/BST)</option>
        <option value="EST5EDT,M3.2.0,M11.1.0" )rawliteral";
  html += timezone == "EST5EDT,M3.2.0,M11.1.0" ? "selected" : "";
  html += R"rawliteral(>New York (EST/EDT)</option>
        <option value="PST8PDT,M3.2.0,M11.1.0" )rawliteral";
  html += timezone == "PST8PDT,M3.2.0,M11.1.0" ? "selected" : "";
  html += R"rawliteral(>Los Angeles (PST/PDT)</option>
        <option value="custom">Benutzerdefiniert...</option>
      </select>
      <input type="text" id="tzCustom" placeholder="z.B. CET-1CEST,M3.5.0,M10.5.0/3" style="display:none;" value=")rawliteral";
  html += timezone;
  html += R"rawliteral(">
      <button onclick="saveTimezone()">Speichern</button>
    </div>

    <!-- Modal für WLAN-Passwort-Eingabe -->
    <div id="modal" style="display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.8);z-index:100;">
      <div style="background:#16213e;margin:15% auto;padding:20px;border-radius:15px;max-width:320px;text-align:center;">
        <h3 id="selectedSSID" style="color:#4ecca3;margin-bottom:1rem;"></h3>
        <input type="password" id="password" placeholder="Passwort" style="margin-bottom:1rem;">
        <div>
          <button onclick="connect()">Verbinden</button>
          <button onclick="closeModal()" style="background:#666;">Abbrechen</button>
        </div>
      </div>
    </div>

    <script>
      var selectedSSID = '';
      var isApMode = false;

      // WLAN-Status aktualisieren
      function updateStatus() {
        fetch('/status')
          .then(r => r.json())
          .then(d => {
            var s = document.getElementById('status');
            isApMode = d.apMode;
            if (d.apMode) {
              // Access Point Modus
              s.innerHTML = '<table>' +
                '<tr><td>Modus</td><td><span class="status disconnected">Access Point</span></td></tr>' +
                '<tr><td>SSID</td><td>' + d.apSSID + '</td></tr>' +
                '<tr><td>Passwort</td><td>' + d.apPassword + '</td></tr>' +
                '<tr><td>IP</td><td>192.168.4.1</td></tr>' +
                '</table>';
              document.getElementById('resetCard').style.display = 'none';
            } else {
              // Station Modus (mit Router verbunden)
              s.innerHTML = '<table>' +
                '<tr><td>Modus</td><td><span class="status connected">Verbunden</span></td></tr>' +
                '<tr><td>SSID</td><td>' + d.ssid + '</td></tr>' +
                '<tr><td>IP Adresse</td><td>' + d.ip + '</td></tr>' +
                '<tr><td>MAC Adresse</td><td>)rawliteral";
  html += macAddress;
  html += R"rawliteral(</td></tr>' +
                '</table>';
              document.getElementById('resetCard').style.display = 'block';
            }
          });
      }

      // Verfügbare Netzwerke scannen
      function scanNetworks() {
        document.getElementById('networks').innerHTML = '<p style="color:#888;">Suche...</p>';
        fetch('/scan')
          .then(r => r.json())
          .then(d => {
            var html = '';
            d.forEach(n => {
              html += '<div style="background:#1a1a2e;padding:1rem;border-radius:8px;margin:0.5rem 0;cursor:pointer;" onclick="selectNetwork(\'' + n.ssid.replace(/'/g, "\\'") + '\')">' +
                '<div style="font-weight:bold;">' + n.ssid + '</div>' +
                '<div style="color:#888;font-size:0.85rem;">Signal: ' + n.rssi + ' dBm</div>' +
                '</div>';
            });
            document.getElementById('networks').innerHTML = html || '<p style="color:#888;">Keine Netzwerke gefunden</p>';
          });
      }

      // Netzwerk auswählen (öffnet Modal)
      function selectNetwork(ssid) {
        selectedSSID = ssid;
        document.getElementById('selectedSSID').textContent = ssid;
        document.getElementById('password').value = '';
        document.getElementById('modal').style.display = 'block';
      }

      // Modal schließen
      function closeModal() {
        document.getElementById('modal').style.display = 'none';
      }

      // Mit ausgewähltem Netzwerk verbinden
      function connect() {
        var pw = document.getElementById('password').value;
        document.getElementById('modal').innerHTML = '<div style="background:#16213e;margin:15% auto;padding:20px;border-radius:15px;max-width:320px;text-align:center;"><p>Verbinde...</p></div>';
        fetch('/connect', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: 'ssid=' + encodeURIComponent(selectedSSID) + '&password=' + encodeURIComponent(pw)
        })
        .then(r => r.json())
        .then(d => {
          closeModal();
          if (d.success) {
            document.getElementById('networks').innerHTML = '<div style="background:#4ecca333;padding:1rem;border-radius:8px;">' +
              '<h3 style="color:#4ecca3;">Verbindung erfolgreich!</h3>' +
              '<p>Neue IP: <strong>' + d.ip + '</strong></p>' +
              '<p>Erreichbar unter: <a href="http://litime-bms.local" style="color:#4ecca3;">http://litime-bms.local</a></p>' +
              '</div>';
            setTimeout(() => location.reload(), 3000);
          } else {
            alert('Verbindung fehlgeschlagen: ' + d.message);
            location.reload();
          }
        });
      }

      // WLAN zurücksetzen
      function resetWiFi() {
        if (confirm('WLAN-Zugangsdaten wirklich löschen?')) {
          fetch('/reset', { method: 'POST' })
            .then(r => r.json())
            .then(d => {
              alert('WLAN-Daten gelöscht. Das Gerät startet im Access Point Modus neu.');
            });
        }
      }

      // Zeitzone speichern
      function saveTimezone() {
        let tz = document.getElementById('timezone').value;
        if (tz === 'custom') {
          tz = document.getElementById('tzCustom').value;
        }
        fetch('/api/timezone', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({timezone: tz})
        }).then(() => alert('Zeitzone gespeichert!'));
      }

      // Status beim Laden aktualisieren
      updateStatus();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

// ============================================================================
// API-Endpunkte
// ============================================================================
// RESTful JSON-APIs für AJAX-Anfragen vom Webinterface

/**
 * GET /api/time - Gibt aktuelle Zeit und Sync-Status zurück
 */
void handleApiTime() {
  JsonDocument doc;
  doc["time"] = getCurrentTimeString();
  doc["lastSync"] = getLastSyncTimeString();
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * GET /api/data - Gibt alle BMS-Daten als JSON zurück
 */
void handleApiData() {
  JsonDocument doc;

  // Verfügbarkeits-Flag für Frontend
  bool bmsAvailable = bluetoothEnabled && bmsConnected && bmsDataValid;
  doc["available"] = bmsAvailable;

  // Alle BMS-Werte
  doc["totalVoltage"] = bmsData.totalVoltage;
  doc["cellVoltageSum"] = bmsData.cellVoltageSum;
  doc["current"] = bmsData.current;
  doc["mosfetTemp"] = bmsData.mosfetTemp;
  doc["cellTemp"] = bmsData.cellTemp;
  doc["soc"] = bmsData.soc;
  doc["soh"] = bmsData.soh;
  doc["remainingAh"] = bmsData.remainingAh;
  doc["fullCapacityAh"] = bmsData.fullCapacityAh;
  doc["protectionState"] = bmsData.protectionState;
  doc["heatState"] = bmsData.heatState;
  doc["failureState"] = bmsData.failureState;
  doc["balancingState"] = bmsData.balancingState;
  doc["batteryState"] = bmsData.batteryState;
  doc["dischargesCount"] = bmsData.dischargesCount;
  doc["dischargesAhCount"] = bmsData.dischargesAhCount;
  doc["connected"] = bmsConnected;

  // Zellspannungen als Array
  JsonArray cells = doc["cellVoltages"].to<JsonArray>();
  for (float v : bmsData.cellVoltages) {
    cells.add(v);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * GET /api/status - Gibt Systemstatus für Statusleiste zurück
 */
void handleApiStatus() {
  JsonDocument doc;

  // WLAN Status
  doc["apMode"] = apMode;
  doc["wlanConnected"] = (WiFi.status() == WL_CONNECTED);

  // Internet-Verbindung (NTP als Indikator)
  doc["internetOk"] = (lastSyncTime > 0);

  // NTP Status
  doc["ntpSynced"] = (lastSyncTime > 0);

  // Terminal/Serial
  doc["serialEnabled"] = serialOutputEnabled;

  // Bluetooth
  doc["btEnabled"] = bluetoothEnabled;

  // BMS Verbindung
  doc["bmsConnected"] = bmsConnected;
  doc["bmsDataValid"] = bmsDataValid;

  // Cloud/Home Assistant
  doc["cloudEnabled"] = haEnabled;
  doc["cloudOk"] = (haEnabled && lastHaHttpCode == 200);

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

/**
 * POST /api/bluetooth - Bluetooth aktivieren/deaktivieren
 *
 * Body: {"enabled": true/false}
 */
void handleApiBluetooth() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    bluetoothEnabled = doc["enabled"].as<bool>();
    saveSettings();

    if (bluetoothEnabled && !bmsConnected && !bmsConnectPending) {
      // BMS-Verbindung wird im loop() non-blocking hergestellt
      bmsConnectPending = true;
      Serial.println("[BLE] BMS-Verbindung angefordert, wird im Hintergrund hergestellt...");
    } else if (!bluetoothEnabled && bmsConnected) {
      // Bluetooth deaktiviert: Verbindung trennen
      bmsClient.disconnect();
      bmsConnected = false;
      bmsConnectPending = false;
    }
  }
  server.send(200, "application/json", "{\"success\":true}");
}

/**
 * POST /api/serial - Terminal-Ausgabe aktivieren/deaktivieren
 *
 * Body: {"enabled": true/false}
 */
void handleApiSerial() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    serialOutputEnabled = doc["enabled"].as<bool>();
    saveSettings();
    Serial.println(serialOutputEnabled ? "[SERIAL] Terminal-Ausgabe aktiviert" : "[SERIAL] Terminal-Ausgabe deaktiviert");
  }
  server.send(200, "application/json", "{\"success\":true}");
}

/**
 * POST /api/bms-settings - BMS-Einstellungen speichern
 *
 * Body: {"mac": "XX:XX:XX:XX:XX:XX", "interval": 20}
 * Bei MAC-Änderung wird das Gerät neu gestartet
 */
void handleApiBmsSettings() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));

    String newMac = doc["mac"].as<String>();
    unsigned long newInterval = doc["interval"].as<unsigned long>();

    // Intervall validieren (5-300 Sekunden)
    if (newInterval < 5) newInterval = 5;
    if (newInterval > 300) newInterval = 300;
    bmsInterval = newInterval;

    // Prüfen ob MAC geändert wurde (muss 17 Zeichen haben: XX:XX:XX:XX:XX:XX)
    bool macChanged = (newMac != bmsMac && newMac.length() == 17);
    if (macChanged) {
      bmsMac = newMac;
    }

    saveSettings();
    server.send(200, "application/json", "{\"success\":true}");

    // Bei MAC-Änderung Neustart erforderlich
    if (macChanged) {
      delay(1000);
      ESP.restart();
    }
  } else {
    server.send(200, "application/json", "{\"success\":true}");
  }
}

/**
 * POST /api/timezone - Zeitzone speichern und NTP neu synchronisieren
 *
 * Body: {"timezone": "CET-1CEST,M3.5.0,M10.5.0/3"}
 */
void handleApiTimezone() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    timezone = doc["timezone"].as<String>();
    saveSettings();
    syncNTP();  // Sofort neu synchronisieren mit neuer Zeitzone
  }
  server.send(200, "application/json", "{\"success\":true}");
}

/**
 * POST /api/ha-settings - Home Assistant Webhook-Einstellungen speichern
 *
 * Body: {"enabled": true, "url": "http://...", "interval": 60}
 */
void handleApiHaSettings() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    haEnabled = doc["enabled"].as<bool>();
    haWebhookUrl = doc["url"].as<String>();
    haInterval = doc["interval"].as<unsigned long>();

    // Intervall validieren (10-3600 Sekunden)
    if (haInterval < 10) haInterval = 10;
    if (haInterval > 3600) haInterval = 3600;

    saveSettings();
  }
  server.send(200, "application/json", "{\"success\":true}");
}

/**
 * POST /api/ha-test - Test-Webhook an Home Assistant senden
 *
 * Sendet sofort einen Webhook unabhängig vom Intervall
 */
void handleApiHaTest() {
  // Im AP-Modus nicht möglich
  if (apMode) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Nicht im AP-Modus möglich\"}");
    return;
  }
  // Keine URL konfiguriert
  if (haWebhookUrl.length() == 0) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Keine Webhook URL konfiguriert\"}");
    return;
  }

  // Temporär aktivieren für Test (falls deaktiviert)
  bool wasEnabled = haEnabled;
  haEnabled = true;
  bool success = sendToHomeAssistant();
  haEnabled = wasEnabled;

  if (success) {
    server.send(200, "application/json", "{\"success\":true}");
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"HTTP Fehler\"}");
  }
}

/**
 * POST /api/reset-wifi - WLAN-Daten löschen und Gerät neu starten
 */
void handleApiResetWifi() {
  // WLAN-Daten aus NVS löschen
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");
  delay(1000);
  ESP.restart();  // Neustart im AP-Modus
}

// ============================================================================
// WLAN-Handler
// ============================================================================

/**
 * GET /scan - Verfügbare WLAN-Netzwerke scannen
 *
 * @return JSON-Array mit {ssid, rssi} für jedes gefundene Netzwerk
 */
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");  // Anführungszeichen escapen
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

/**
 * GET /status - WLAN-Status abfragen
 *
 * @return JSON mit apMode, ssid, ip oder apSSID, apPassword
 */
void handleStatus() {
  String json;
  if (apMode) {
    json = "{\"apMode\":true,\"apSSID\":\"" + apSSID + "\",\"apPassword\":\"" + String(AP_PASSWORD) + "\"}";
  } else {
    json = "{\"apMode\":false,\"ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  }
  server.send(200, "application/json", json);
}

/**
 * POST /connect - Mit neuem WLAN-Netzwerk verbinden
 *
 * Parameter: ssid, password (application/x-www-form-urlencoded)
 */
void handleConnect() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  Serial.println("Verbinde mit: " + ssid);

  // In Station-Modus wechseln
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_5dBm);  // Niedrige Sendeleistung gegen Überhitzung
  WiFi.begin(ssid.c_str(), password.c_str());

  // Auf Verbindung warten (max. 15 Sekunden)
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Credentials im NVS speichern
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();

    apMode = false;

    // mDNS starten für litime-bms.local
    MDNS.begin("litime-bms");

    String json = "{\"success\":true,\"ip\":\"" + WiFi.localIP().toString() + "\",\"ssid\":\"" + ssid + "\"}";
    server.send(200, "application/json", json);
    Serial.println("\nVerbunden! IP: " + WiFi.localIP().toString());
  } else {
    // Verbindung fehlgeschlagen: zurück in AP-Modus
    startAP();
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Verbindung fehlgeschlagen\"}");
  }
}

/**
 * POST /reset - WLAN-Daten löschen und Gerät neu starten
 */
void handleReset() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");

  delay(1000);
  ESP.restart();
}

// ============================================================================
// WLAN-Funktionen
// ============================================================================

/**
 * Startet den Access Point Modus für die Erstkonfiguration
 *
 * Der AP wird mit der SSID "LiTime-BMS-XXXX" erstellt,
 * wobei XXXX die letzten 4 Zeichen der MAC-Adresse sind.
 */
void startAP() {
  Serial.println("[AP] Starte Access Point...");
  Serial.println("[AP] SSID: " + apSSID);
  Serial.println("[AP] Passwort: " + String(AP_PASSWORD));

  // WiFi komplett zurücksetzen für sauberen Start
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // In AP-Modus wechseln
  Serial.println("[AP] WiFi Mode setzen auf WIFI_AP...");
  bool modeOk = WiFi.mode(WIFI_AP);
  Serial.println("[AP] WiFi.mode(WIFI_AP) = " + String(modeOk ? "OK" : "FEHLER"));
  delay(200);

  // Sendeleistung reduzieren um Wärmeentwicklung zu minimieren
  WiFi.setTxPower(WIFI_POWER_5dBm);
  Serial.println("[AP] TX Power gesetzt auf 5 dBm");

  // IP-Konfiguration VOR softAP setzen
  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  Serial.println("[AP] Setze IP-Konfiguration...");
  bool configOk = WiFi.softAPConfig(apIP, gateway, subnet);
  Serial.println("[AP] WiFi.softAPConfig() = " + String(configOk ? "OK" : "FEHLER"));

  // Access Point starten
  // Kanal 6 ist oft weniger überlastet
  // Nicht versteckt, maximal 4 gleichzeitige Clients
  Serial.println("[AP] Starte softAP auf Kanal 6...");
  bool apOk = WiFi.softAP(apSSID.c_str(), AP_PASSWORD, 6, false, 4);
  Serial.println("[AP] WiFi.softAP() = " + String(apOk ? "OK" : "FEHLER"));

  delay(1000);  // Warten bis AP stabil ist

  apMode = true;

  // Detaillierten Status ausgeben
  Serial.println("[AP] ══════════════════════════════════════");
  Serial.println("[AP] Access Point Status:");
  Serial.println("[AP]   SSID: " + apSSID);
  Serial.println("[AP]   Passwort: " + String(AP_PASSWORD));
  Serial.println("[AP]   IP: " + WiFi.softAPIP().toString());
  Serial.println("[AP]   Kanal: 6");
  Serial.println("[AP]   MAC: " + WiFi.softAPmacAddress());
  Serial.println("[AP]   TX Power: " + String(WiFi.getTxPower()) + " (0.25 dBm units)");
  Serial.println("[AP]   Clients: " + String(WiFi.softAPgetStationNum()));
  Serial.println("[AP] ══════════════════════════════════════");
}

/**
 * Versucht eine Verbindung mit dem gespeicherten WLAN-Netzwerk herzustellen
 *
 * @return true wenn Verbindung erfolgreich, false wenn fehlgeschlagen oder keine Daten gespeichert
 */
bool connectToSavedWiFi() {
  Serial.println("[WIFI] Prüfe gespeicherte WLAN-Daten...");

  // Gespeicherte Credentials aus NVS laden
  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();

  Serial.println("[WIFI] Gespeicherte SSID: '" + ssid + "'");
  Serial.println("[WIFI] Passwort-Länge: " + String(password.length()));

  // Keine Daten vorhanden
  if (ssid.length() == 0) {
    Serial.println("[WIFI] Keine gespeicherten WLAN-Daten gefunden");
    return false;
  }

  Serial.println("[WIFI] Verbinde mit gespeichertem WLAN: " + ssid);

  // In Station-Modus wechseln
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_5dBm);  // Niedrige Sendeleistung gegen Überhitzung
  WiFi.begin(ssid.c_str(), password.c_str());

  // Auf Verbindung warten mit Fortschrittsanzeige
  unsigned long start = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
    dots++;
    // Alle 20 Punkte Status ausgeben
    if (dots % 20 == 0) {
      Serial.println();
      Serial.print("[WIFI] Status: ");
      Serial.println(WiFi.status());
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Verbindung erfolgreich
    Serial.println();
    Serial.println("[WIFI] ══════════════════════════════════════");
    Serial.println("[WIFI] Verbunden!");
    Serial.println("[WIFI]   SSID: " + WiFi.SSID());
    Serial.println("[WIFI]   IP: " + WiFi.localIP().toString());
    Serial.println("[WIFI]   Gateway: " + WiFi.gatewayIP().toString());
    Serial.println("[WIFI]   RSSI: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("[WIFI] ══════════════════════════════════════");

    // mDNS starten für litime-bms.local
    if (MDNS.begin("litime-bms")) {
      Serial.println("[WIFI] mDNS gestartet: http://litime-bms.local");
    }

    return true;
  }

  // Verbindung fehlgeschlagen
  Serial.println();
  Serial.println("[WIFI] Verbindung fehlgeschlagen! Status: " + String(WiFi.status()));
  return false;
}

// ============================================================================
// Webserver-Setup
// ============================================================================

/**
 * Konfiguriert alle Routen des Webservers
 *
 * Routen:
 * - /              - Hauptseite (Werte)
 * - /bluetooth     - Bluetooth-Einstellungen
 * - /cloud         - Home Assistant Einstellungen
 * - /wlan          - WLAN-Einstellungen
 * - /api/*         - JSON-APIs
 * - /scan, /status, /connect, /reset - WLAN-Konfiguration
 */
void setupWebServer() {
  // Hauptseiten
  server.on("/", handleRoot);
  server.on("/bluetooth", handleBluetooth);
  server.on("/cloud", handleCloud);
  server.on("/wlan", handleWlan);

  // API Endpunkte für AJAX
  server.on("/api/time", HTTP_GET, handleApiTime);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/bluetooth", HTTP_POST, handleApiBluetooth);
  server.on("/api/serial", HTTP_POST, handleApiSerial);
  server.on("/api/bms-settings", HTTP_POST, handleApiBmsSettings);
  server.on("/api/timezone", HTTP_POST, handleApiTimezone);
  server.on("/api/reset-wifi", HTTP_POST, handleApiResetWifi);
  server.on("/api/ha-settings", HTTP_POST, handleApiHaSettings);
  server.on("/api/ha-test", HTTP_POST, handleApiHaTest);

  // WLAN-Konfiguration
  server.on("/scan", handleScan);
  server.on("/status", handleStatus);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/reset", HTTP_POST, handleReset);

  server.begin();
  Serial.println("Webserver gestartet");
}

// ============================================================================
// Setup - Wird einmal beim Start ausgeführt
// ============================================================================

/**
 * Initialisiert alle Komponenten beim Start
 *
 * Reihenfolge:
 * 1. Serial-Kommunikation starten
 * 2. NVS (Non-Volatile Storage) initialisieren
 * 3. Benutzereinstellungen laden
 * 4. MAC-Adresse auslesen und AP-SSID erstellen
 * 5. WLAN-Verbindung herstellen oder AP starten
 * 6. Webserver starten
 * 7. NTP synchronisieren (wenn WLAN verbunden)
 * 8. BMS-Verbindung herstellen (wenn konfiguriert)
 */
void setup() {
  // Serial-Kommunikation mit 115200 Baud starten
  Serial.begin(115200);
  delay(2000);  // Warten bis Serial bereit ist (USB CDC braucht etwas Zeit)

  // Willkommensnachricht
  Serial.println();
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("         LiTime LiFePO4 BMS Monitor gestartet         ");
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println();

  // NVS (Non-Volatile Storage) initialisieren
  // Behebt den Fehler "nvs_open failed: NOT_FOUND"
  Serial.println("[INIT] Initialisiere NVS...");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // NVS beschädigt: löschen und neu initialisieren
    Serial.println("[INIT] NVS löschen und neu initialisieren...");
    nvs_flash_erase();
    ret = nvs_flash_init();
  }
  Serial.println("[INIT] NVS Status: " + String(ret == ESP_OK ? "OK" : "FEHLER"));

  // ESP32 Chip-Informationen ausgeben (für Debugging)
  Serial.println("[INIT] ESP32 Chip Info:");
  Serial.println("[INIT]   Chip Model: " + String(ESP.getChipModel()));
  Serial.println("[INIT]   Chip Rev: " + String(ESP.getChipRevision()));
  Serial.println("[INIT]   CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz");
  Serial.println("[INIT]   Flash Size: " + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB");
  Serial.println("[INIT]   Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println();

  // Benutzereinstellungen aus NVS laden
  Serial.println("[INIT] Lade Einstellungen...");
  loadSettings();
  Serial.println("[INIT] Einstellungen geladen");

  // MAC-Adresse auslesen für eindeutige AP-SSID
  Serial.println("[INIT] Lese MAC-Adresse...");
  macAddress = WiFi.macAddress();
  // Letzten 4 Zeichen der MAC für SSID verwenden (ohne Doppelpunkte)
  String macSuffix = macAddress.substring(macAddress.length() - 5);
  macSuffix.replace(":", "");
  apSSID = "LiTime-BMS-" + macSuffix;

  Serial.println("[INIT] MAC: " + macAddress);
  Serial.println("[INIT] AP-SSID wird: " + apSSID);
  Serial.println();

  // WLAN-Verbindung herstellen oder Access Point starten
  Serial.println("[INIT] Starte WLAN...");
  if (!connectToSavedWiFi()) {
    // Keine gespeicherten Daten oder Verbindung fehlgeschlagen
    Serial.println("[INIT] Kein WLAN -> starte AP");
    startAP();
  } else {
    Serial.println("[INIT] WLAN verbunden");
  }

  Serial.println();
  Serial.println("[INIT] Aktueller Modus: " + String(apMode ? "ACCESS POINT" : "STATION"));
  Serial.println();

  // Webserver mit allen Routen starten
  Serial.println("[INIT] Starte Webserver...");
  setupWebServer();

  // NTP-Zeitsynchronisation (nur wenn mit Router verbunden)
  if (!apMode) {
    Serial.println("[INIT] Synchronisiere NTP...");
    syncNTP();
  } else {
    Serial.println("[INIT] AP-Modus - überspringe NTP");
  }

  // BMS-Verbindung wenn Bluetooth aktiviert und MAC konfiguriert
  if (bluetoothEnabled && bmsMac.length() == 17) {
    Serial.println("[INIT] Verbinde mit BMS: " + bmsMac);
    bmsClient.init(bmsMac.c_str());
    bmsConnected = bmsClient.connect();
    if (bmsConnected) {
      Serial.println("[INIT] BMS verbunden!");
      updateBMSData();  // Erste Datenabfrage
    } else {
      Serial.println("[INIT] BMS Verbindung fehlgeschlagen!");
    }
  } else if (bluetoothEnabled && bmsMac.length() != 17) {
    Serial.println("[INIT] BMS MAC nicht konfiguriert - bitte im Webinterface einstellen");
  }

  // Timing-Variablen initialisieren
  lastBmsUpdate = millis();
  lastNtpSync = millis();

  // Setup abgeschlossen - Benutzerhinweise ausgeben
  Serial.println();
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("[INIT] Setup abgeschlossen!");
  if (apMode) {
    // Anleitung für Erstkonfiguration
    Serial.println("[INIT] Verbinde dich mit WLAN: " + apSSID);
    Serial.println("[INIT] Passwort: " + String(AP_PASSWORD));
    Serial.println("[INIT] Dann öffne: http://192.168.4.1");
  } else {
    // Webinterface-URL anzeigen
    Serial.println("[INIT] Webinterface: http://" + WiFi.localIP().toString());
  }
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println();
}

// ============================================================================
// Loop - Hauptschleife (wird kontinuierlich ausgeführt)
// ============================================================================

/**
 * Hauptschleife - verarbeitet alle zyklischen Aufgaben
 *
 * Alle Operationen sind non-blocking implementiert um den
 * Webserver nicht zu blockieren. Timing erfolgt über millis().
 *
 * Aufgaben:
 * 1. Webserver-Anfragen verarbeiten
 * 2. WLAN-Verbindung überwachen und bei Bedarf reconnecten
 * 3. BMS-Daten periodisch abfragen
 * 4. BMS-Reconnect bei Verbindungsverlust
 * 5. NTP periodisch synchronisieren
 * 6. Home Assistant Webhook periodisch senden
 */
void loop() {
  unsigned long currentMillis = millis();

  // ========================================
  // Webserver-Anfragen verarbeiten
  // ========================================
  // Muss in jeder Loop-Iteration aufgerufen werden
  server.handleClient();

  // ========================================
  // WLAN-Verbindung überwachen (non-blocking)
  // ========================================
  // Nur im Station-Modus (nicht im AP-Modus)
  if (!apMode) {
    // Wenn Reconnect läuft: Status prüfen
    if (wifiReconnecting) {
      if (WiFi.status() == WL_CONNECTED) {
        // Reconnect erfolgreich
        Serial.println("[WIFI] Reconnect erfolgreich! IP: " + WiFi.localIP().toString());
        MDNS.begin("litime-bms");  // mDNS neu starten
        wifiReconnecting = false;
      } else if (currentMillis - wifiReconnectStart > 15000) {
        // Timeout nach 15 Sekunden
        Serial.println("[WIFI] Reconnect Timeout, versuche erneut beim nächsten Check");
        wifiReconnecting = false;
      }
    }
    // Periodisch Verbindung prüfen (alle 30 Sekunden)
    else if (currentMillis - lastWifiCheck >= wifiCheckInterval) {
      lastWifiCheck = currentMillis;
      if (WiFi.status() != WL_CONNECTED) {
        // Verbindung verloren: Reconnect starten
        Serial.println("[WIFI] Verbindung verloren, starte Reconnect...");
        WiFi.reconnect();
        wifiReconnecting = true;
        wifiReconnectStart = currentMillis;
      }
    }
  }

  // ========================================
  // BMS-Verbindung herstellen (non-blocking)
  // ========================================
  // Wird vom API-Handler oder Reconnect-Timer angefordert
  if (bluetoothEnabled && bmsConnectPending && !bmsConnected && bmsMac.length() == 17) {
    Serial.println("[BLE] Stelle BMS-Verbindung her...");
    bmsClient.init(bmsMac.c_str());
    bmsConnected = bmsClient.connect();
    bmsConnectPending = false;
    if (bmsConnected) {
      Serial.println("[BLE] BMS-Verbindung erfolgreich!");
      updateBMSData();
    } else {
      Serial.println("[BLE] BMS-Verbindung fehlgeschlagen");
    }
    lastBmsUpdate = currentMillis;
  } else if (bmsConnectPending && bmsMac.length() != 17) {
    // Keine MAC konfiguriert: Request verwerfen
    bmsConnectPending = false;
  }

  // ========================================
  // BMS-Daten periodisch abfragen
  // ========================================
  // Funktioniert auch im AP-Modus
  if (bluetoothEnabled && bmsConnected && (currentMillis - lastBmsUpdate >= bmsInterval * 1000)) {
    updateBMSData();
    lastBmsUpdate = currentMillis;
  }

  // ========================================
  // BMS-Reconnect bei Verbindungsverlust
  // ========================================
  // Alle 30 Sekunden erneut versuchen
  if (bluetoothEnabled && !bmsConnected && !bmsConnectPending && (currentMillis - lastBmsUpdate >= 30000)) {
    Serial.println("[BLE] Plane BMS Reconnect...");
    bmsConnectPending = true;  // Wird im nächsten Loop-Durchlauf ausgeführt
  }

  // ========================================
  // NTP periodisch synchronisieren
  // ========================================
  // Nur wenn WLAN verbunden, alle 1 Stunde
  if (!apMode && WiFi.status() == WL_CONNECTED && (currentMillis - lastNtpSync >= ntpSyncInterval)) {
    syncNTP();
    lastNtpSync = currentMillis;
  }

  // ========================================
  // Home Assistant Webhook periodisch senden
  // ========================================
  // Nur wenn aktiviert und WLAN verbunden
  if (haEnabled && !apMode && WiFi.status() == WL_CONNECTED && (currentMillis - lastHaSend >= haInterval * 1000)) {
    sendToHomeAssistant();
    lastHaSend = currentMillis;
  }
}

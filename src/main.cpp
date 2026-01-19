#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <BMSClient.h>
#include <time.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>
#include <HTTPClient.h>

// WLAN Konfiguration
#define AP_PASSWORD "12345678"
#define WIFI_TIMEOUT_MS 30000

// BMS Client
BMSClient bmsClient;
String bmsMac = "C8:47:80:3F:67:7C";  // Standard-MAC, kann über Webinterface geändert werden

// Webserver
WebServer server(80);

// Preferences für persistente Einstellungen
Preferences preferences;

// WLAN Status
String macAddress;
String apSSID;
bool apMode = false;

// Einstellungen
String timezone = "CET-1CEST,M3.5.0,M10.5.0/3";  // Berlin mit Sommerzeit
unsigned long bmsInterval = 20;  // Sekunden
bool bluetoothEnabled = true;
bool bmsConnected = false;
bool serialOutputEnabled = true;  // Terminal-Ausgabe für BMS-Daten
bool bmsDataValid = false;  // Daten sind plausibel

// Home Assistant Webhook
String haWebhookUrl = "";
unsigned long haInterval = 60;  // Sekunden
bool haEnabled = false;
unsigned long lastHaSend = 0;
String lastHaTime = "";
int lastHaHttpCode = 0;
String lastHaResponse = "";

// Timing ohne delay
unsigned long lastBmsUpdate = 0;
unsigned long lastNtpSync = 0;
unsigned long ntpSyncInterval = 3600000;  // 1 Stunde
time_t lastSyncTime = 0;
unsigned long lastWifiCheck = 0;
unsigned long wifiCheckInterval = 30000;  // WLAN alle 30 Sekunden prüfen

// NTP Server
const char* ntpServer = "pool.ntp.org";

// Gespeicherte BMS Daten
struct BMSData {
  float totalVoltage = 0;
  float cellVoltageSum = 0;
  float current = 0;
  int16_t mosfetTemp = 0;
  int16_t cellTemp = 0;
  uint8_t soc = 0;
  String soh = "";
  float remainingAh = 0;
  float fullCapacityAh = 0;
  String protectionState = "";
  String heatState = "";
  String balanceMemory = "";
  String failureState = "";
  String balancingState = "";
  String batteryState = "";
  uint32_t dischargesCount = 0;
  float dischargesAhCount = 0;
  std::vector<float> cellVoltages;
} bmsData;

// Forward declarations
void printBMSDataSerial();
void startAP();
bool connectToSavedWiFi();

void saveSettings() {
  preferences.begin("settings", false);
  preferences.putString("timezone", timezone);
  preferences.putULong("bmsInterval", bmsInterval);
  preferences.putBool("btEnabled", bluetoothEnabled);
  preferences.putString("bmsMac", bmsMac);
  preferences.putString("haWebhook", haWebhookUrl);
  preferences.putULong("haInterval", haInterval);
  preferences.putBool("haEnabled", haEnabled);
  preferences.putBool("serialOut", serialOutputEnabled);
  preferences.end();
}

void loadSettings() {
  preferences.begin("settings", true);
  timezone = preferences.getString("timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
  bmsInterval = preferences.getULong("bmsInterval", 20);
  bluetoothEnabled = preferences.getBool("btEnabled", true);
  bmsMac = preferences.getString("bmsMac", "C8:47:80:3F:67:7C");
  haWebhookUrl = preferences.getString("haWebhook", "");
  haInterval = preferences.getULong("haInterval", 60);
  haEnabled = preferences.getBool("haEnabled", false);
  serialOutputEnabled = preferences.getBool("serialOut", true);
  preferences.end();
}

// Prüft ob BMS-Daten plausibel sind
bool isBmsDataValid() {
  // Spannung muss zwischen 10V und 60V liegen (typisch für LiFePO4)
  if (bmsData.totalVoltage < 10.0 || bmsData.totalVoltage > 60.0) {
    return false;
  }
  // SOC muss zwischen 0 und 100 sein
  if (bmsData.soc > 100) {
    return false;
  }
  // Mindestens eine Zellspannung muss vorhanden sein
  if (bmsData.cellVoltages.size() == 0) {
    return false;
  }
  // Zellspannungen müssen plausibel sein (2.5V - 3.65V für LiFePO4)
  for (float v : bmsData.cellVoltages) {
    if (v < 2.0 || v > 4.0) {
      return false;
    }
  }
  return true;
}

void syncNTP() {
  configTzTime(timezone.c_str(), ntpServer);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    lastSyncTime = time(nullptr);
    Serial.println("NTP synchronisiert");
  }
}

String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Zeit nicht verfügbar";
  }
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

String getLastSyncTimeString() {
  if (lastSyncTime == 0) {
    return "Noch nicht synchronisiert";
  }
  struct tm* timeinfo = localtime(&lastSyncTime);
  char buffer[64];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", timeinfo);
  return String(buffer);
}

void updateBMSData() {
  if (!bmsConnected) return;

  bmsClient.update();
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

  // Plausibilitätsprüfung
  bool wasValid = bmsDataValid;
  bmsDataValid = isBmsDataValid();

  if (!bmsDataValid) {
    if (wasValid) {
      Serial.println("[BMS] Daten nicht plausibel - überspringe Ausgabe/Webhook");
    }
    return;  // Keine weitere Verarbeitung bei ungültigen Daten
  }

  if (!wasValid && bmsDataValid) {
    Serial.println("[BMS] Daten jetzt plausibel - Ausgabe aktiviert");
  }

  if (serialOutputEnabled) {
    printBMSDataSerial();
  }
}

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

// HTML Templates
const char* HTML_HEADER = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>LiTime BMS Monitor</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #1a1a2e; color: #eee; min-height: 100vh; }
    .nav { background: #16213e; padding: 1rem; display: flex; gap: 1rem; flex-wrap: wrap; }
    .nav a { color: #4ecca3; text-decoration: none; padding: 0.5rem 1rem; border-radius: 5px; transition: background 0.3s; }
    .nav a:hover, .nav a.active { background: #4ecca3; color: #1a1a2e; }
    .container { max-width: 900px; margin: 0 auto; padding: 1rem; }
    .card { background: #16213e; border-radius: 10px; padding: 1.5rem; margin-bottom: 1rem; }
    .card h2 { color: #4ecca3; margin-bottom: 1rem; border-bottom: 1px solid #4ecca3; padding-bottom: 0.5rem; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; }
    .stat { background: #1a1a2e; padding: 1rem; border-radius: 8px; text-align: center; }
    .stat-value { font-size: 1.8rem; font-weight: bold; color: #4ecca3; }
    .stat-label { font-size: 0.9rem; color: #888; margin-top: 0.3rem; }
    input, select { width: 100%; padding: 0.8rem; margin: 0.5rem 0; border: 1px solid #4ecca3; border-radius: 5px; background: #1a1a2e; color: #eee; }
    button { background: #4ecca3; color: #1a1a2e; padding: 0.8rem 1.5rem; border: none; border-radius: 5px; cursor: pointer; font-weight: bold; margin-top: 0.5rem; }
    button:hover { background: #3db892; }
    .toggle { display: flex; align-items: center; gap: 1rem; }
    .toggle-switch { position: relative; width: 60px; height: 30px; }
    .toggle-switch input { opacity: 0; width: 0; height: 0; }
    .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #ccc; border-radius: 30px; transition: 0.4s; }
    .slider:before { position: absolute; content: ""; height: 22px; width: 22px; left: 4px; bottom: 4px; background: white; border-radius: 50%; transition: 0.4s; }
    input:checked + .slider { background: #4ecca3; }
    input:checked + .slider:before { transform: translateX(30px); }
    .status { padding: 0.3rem 0.8rem; border-radius: 15px; font-size: 0.85rem; }
    .status.connected { background: #4ecca3; color: #1a1a2e; }
    .status.disconnected { background: #e74c3c; color: white; }
    .time-display { font-size: 2rem; font-weight: bold; color: #4ecca3; text-align: center; padding: 1rem; }
    table { width: 100%; border-collapse: collapse; }
    td { padding: 0.5rem; border-bottom: 1px solid #333; }
    td:first-child { color: #888; }
    td:last-child { text-align: right; color: #4ecca3; }
    .cell-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(80px, 1fr)); gap: 0.5rem; }
    .cell { background: #1a1a2e; padding: 0.5rem; border-radius: 5px; text-align: center; font-size: 0.85rem; }
    .cell-num { color: #888; font-size: 0.75rem; }
  </style>
</head>
<body>
  <nav class="nav">
    <a href="/" id="nav-values">Werte</a>
    <a href="/bluetooth" id="nav-bluetooth">Bluetooth</a>
    <a href="/cloud" id="nav-cloud">Cloud</a>
    <a href="/wlan" id="nav-wlan">WLAN</a>
  </nav>
  <div class="container">
)rawliteral";

const char* HTML_FOOTER = R"rawliteral(
  </div>
  <script>
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

void handleRoot() {
  String html = HTML_HEADER;

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

    <div class="card">
      <h2>BMS Übersicht</h2>
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
      </div>
    </div>

    <div class="card">
      <h2>Detaillierte Werte</h2>
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
      </table>
    </div>

    <div class="card">
      <h2>Zellspannungen</h2>
      <div class="cell-grid" id="cellGrid">)rawliteral";

  for (size_t i = 0; i < bmsData.cellVoltages.size(); i++) {
    html += "<div class=\"cell\"><div class=\"cell-num\">Zelle " + String(i + 1) + "</div>";
    html += String(bmsData.cellVoltages[i], 3) + " V</div>";
  }

  html += R"rawliteral(
      </div>
    </div>

    <script>
      function updateTime() {
        fetch('/api/time').then(r => r.json()).then(data => {
          document.getElementById('currentTime').textContent = data.time;
        });
      }
      function updateData() {
        fetch('/api/data').then(r => r.json()).then(data => {
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

          let cellHtml = '';
          data.cellVoltages.forEach((v, i) => {
            cellHtml += '<div class="cell"><div class="cell-num">Zelle ' + (i+1) + '</div>' + v.toFixed(3) + ' V</div>';
          });
          document.getElementById('cellGrid').innerHTML = cellHtml;
        });
      }
      setInterval(updateTime, 1000);
      setInterval(updateData, 1000);
      updateTime();
      updateData();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

void handleBluetooth() {
  String html = HTML_HEADER;

  html += R"rawliteral(
    <div class="card">
      <h2>Bluetooth Verbindung</h2>
      <div style="display: flex; align-items: center; gap: 1rem; margin-bottom: 1rem;">
        <span>Status:</span>
        <span class="status )rawliteral";
  html += bmsConnected ? "connected\">Verbunden" : "disconnected\">Getrennt";
  html += R"rawliteral(</span>
      </div>
      <div class="toggle">
        <span>Bluetooth aktivieren</span>
        <label class="toggle-switch">
          <input type="checkbox" id="btToggle" )rawliteral";
  html += bluetoothEnabled ? "checked" : "";
  html += R"rawliteral( onchange="toggleBluetooth(this.checked)">
          <span class="slider"></span>
        </label>
      </div>
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
      function toggleSerial(enabled) {
        fetch('/api/serial', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled})
        });
      }
      function toggleBluetooth(enabled) {
        fetch('/api/bluetooth', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled})
        }).then(() => location.reload());
      }
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

// Home Assistant Webhook senden
bool sendToHomeAssistant() {
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

  HTTPClient http;
  http.setTimeout(10000);  // 10 Sekunden Timeout
  http.setConnectTimeout(5000);  // 5 Sekunden Connect-Timeout
  http.begin(haWebhookUrl);
  http.addHeader("Content-Type", "application/json");

  // JSON erstellen
  JsonDocument doc;
  doc["device"] = "litime-bms";
  doc["mac"] = macAddress;
  doc["timestamp"] = getCurrentTimeString();
  doc["connected"] = bmsConnected;

  // BMS Daten
  JsonObject battery = doc["battery"].to<JsonObject>();
  battery["voltage"] = bmsData.totalVoltage;
  battery["current"] = bmsData.current;
  battery["soc"] = bmsData.soc;
  battery["soh"] = bmsData.soh;
  battery["remaining_ah"] = bmsData.remainingAh;
  battery["full_capacity_ah"] = bmsData.fullCapacityAh;

  // Temperaturen
  JsonObject temps = doc["temperature"].to<JsonObject>();
  temps["mosfet"] = bmsData.mosfetTemp;
  temps["cells"] = bmsData.cellTemp;

  // Status
  JsonObject status = doc["status"].to<JsonObject>();
  status["battery_state"] = bmsData.batteryState;
  status["protection_state"] = bmsData.protectionState;
  status["failure_state"] = bmsData.failureState;
  status["heat_state"] = bmsData.heatState;

  // Zellspannungen
  JsonArray cells = doc["cell_voltages"].to<JsonArray>();
  for (float v : bmsData.cellVoltages) {
    cells.add(v);
  }

  // Statistiken
  JsonObject stats = doc["statistics"].to<JsonObject>();
  stats["discharge_cycles"] = bmsData.dischargesCount;
  stats["discharged_ah"] = bmsData.dischargesAhCount;

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  // Status speichern
  lastHaTime = getCurrentTimeString();
  lastHaHttpCode = httpCode;

  if (httpCode > 0) {
    lastHaResponse = http.getString();
    if (lastHaResponse.length() > 200) {
      lastHaResponse = lastHaResponse.substring(0, 200) + "...";
    }
  } else {
    // Negative Codes sind Fehler (z.B. -1 = Connection refused, -11 = Timeout)
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

  if (httpCode == 200) {
    Serial.println("[HA] Daten erfolgreich gesendet");
    return true;
  } else {
    Serial.printf("[HA] Fehler: HTTP %d - %s\n", httpCode, lastHaResponse.c_str());
    return false;
  }
}

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
      function toggleHA(enabled) {
        fetch('/api/ha-settings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({enabled: enabled, url: document.getElementById('haWebhook').value, interval: parseInt(document.getElementById('haInterval').value)})
        }).then(() => location.reload());
      }

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

      function testHA() {
        fetch('/api/ha-test', {method: 'POST'})
          .then(r => r.json())
          .then(d => {
            location.reload();
          });
      }

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

      refreshPreview();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

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

    <!-- Modal für Passwort-Eingabe -->
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

      function updateStatus() {
        fetch('/status')
          .then(r => r.json())
          .then(d => {
            var s = document.getElementById('status');
            isApMode = d.apMode;
            if (d.apMode) {
              s.innerHTML = '<table>' +
                '<tr><td>Modus</td><td><span class="status disconnected">Access Point</span></td></tr>' +
                '<tr><td>SSID</td><td>' + d.apSSID + '</td></tr>' +
                '<tr><td>Passwort</td><td>' + d.apPassword + '</td></tr>' +
                '<tr><td>IP</td><td>192.168.4.1</td></tr>' +
                '</table>';
              document.getElementById('resetCard').style.display = 'none';
            } else {
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

      function selectNetwork(ssid) {
        selectedSSID = ssid;
        document.getElementById('selectedSSID').textContent = ssid;
        document.getElementById('password').value = '';
        document.getElementById('modal').style.display = 'block';
      }

      function closeModal() {
        document.getElementById('modal').style.display = 'none';
      }

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

      function resetWiFi() {
        if (confirm('WLAN-Zugangsdaten wirklich löschen?')) {
          fetch('/reset', { method: 'POST' })
            .then(r => r.json())
            .then(d => {
              alert('WLAN-Daten gelöscht. Das Gerät startet im Access Point Modus neu.');
            });
        }
      }

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

      updateStatus();
    </script>
  )rawliteral";

  html += HTML_FOOTER;
  server.send(200, "text/html", html);
}

void handleApiTime() {
  JsonDocument doc;
  doc["time"] = getCurrentTimeString();
  doc["lastSync"] = getLastSyncTimeString();
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleApiData() {
  JsonDocument doc;
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

  JsonArray cells = doc["cellVoltages"].to<JsonArray>();
  for (float v : bmsData.cellVoltages) {
    cells.add(v);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleApiBluetooth() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    bluetoothEnabled = doc["enabled"].as<bool>();
    saveSettings();

    if (bluetoothEnabled && !bmsConnected) {
      bmsClient.init(bmsMac.c_str());
      bmsConnected = bmsClient.connect();
    } else if (!bluetoothEnabled && bmsConnected) {
      bmsClient.disconnect();
      bmsConnected = false;
    }
  }
  server.send(200, "application/json", "{\"success\":true}");
}

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

void handleApiBmsSettings() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));

    String newMac = doc["mac"].as<String>();
    unsigned long newInterval = doc["interval"].as<unsigned long>();

    // Intervall validieren
    if (newInterval < 5) newInterval = 5;
    if (newInterval > 300) newInterval = 300;
    bmsInterval = newInterval;

    // Prüfen ob MAC geändert wurde
    bool macChanged = (newMac != bmsMac && newMac.length() == 17);
    if (macChanged) {
      bmsMac = newMac;
    }

    saveSettings();
    server.send(200, "application/json", "{\"success\":true}");

    // Bei MAC-Änderung neu starten
    if (macChanged) {
      delay(1000);
      ESP.restart();
    }
  } else {
    server.send(200, "application/json", "{\"success\":true}");
  }
}

void handleApiTimezone() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    timezone = doc["timezone"].as<String>();
    saveSettings();
    syncNTP();
  }
  server.send(200, "application/json", "{\"success\":true}");
}

void handleApiHaSettings() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    deserializeJson(doc, server.arg("plain"));
    haEnabled = doc["enabled"].as<bool>();
    haWebhookUrl = doc["url"].as<String>();
    haInterval = doc["interval"].as<unsigned long>();
    if (haInterval < 10) haInterval = 10;
    if (haInterval > 3600) haInterval = 3600;
    saveSettings();
  }
  server.send(200, "application/json", "{\"success\":true}");
}

void handleApiHaTest() {
  if (apMode) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Nicht im AP-Modus möglich\"}");
    return;
  }
  if (haWebhookUrl.length() == 0) {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Keine Webhook URL konfiguriert\"}");
    return;
  }

  // Temporär aktivieren für Test
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

void handleApiResetWifi() {
  // WLAN-Daten löschen
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");
  delay(1000);
  ESP.restart();
}

// WLAN Scan Handler
void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// WLAN Status Handler
void handleStatus() {
  String json;
  if (apMode) {
    json = "{\"apMode\":true,\"apSSID\":\"" + apSSID + "\",\"apPassword\":\"" + String(AP_PASSWORD) + "\"}";
  } else {
    json = "{\"apMode\":false,\"ssid\":\"" + WiFi.SSID() + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  }
  server.send(200, "application/json", json);
}

// WLAN Connect Handler
void handleConnect() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  Serial.println("Verbinde mit: " + ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Credentials speichern
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();

    apMode = false;

    // mDNS starten
    MDNS.begin("litime-bms");

    String json = "{\"success\":true,\"ip\":\"" + WiFi.localIP().toString() + "\",\"ssid\":\"" + ssid + "\"}";
    server.send(200, "application/json", json);
    Serial.println("\nVerbunden! IP: " + WiFi.localIP().toString());
  } else {
    // Zurück in AP-Modus
    startAP();
    server.send(200, "application/json", "{\"success\":false,\"message\":\"Verbindung fehlgeschlagen\"}");
  }
}

// WLAN Reset Handler
void handleReset() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();

  server.send(200, "application/json", "{\"success\":true}");

  delay(1000);
  ESP.restart();
}

// Access Point starten
void startAP() {
  Serial.println("[AP] Starte Access Point...");
  Serial.println("[AP] SSID: " + apSSID);
  Serial.println("[AP] Passwort: " + String(AP_PASSWORD));

  // WiFi komplett zurücksetzen
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  Serial.println("[AP] WiFi Mode setzen auf WIFI_AP...");
  bool modeOk = WiFi.mode(WIFI_AP);
  Serial.println("[AP] WiFi.mode(WIFI_AP) = " + String(modeOk ? "OK" : "FEHLER"));
  delay(200);

  // TX Power auf Maximum setzen (ESP32-C3 SuperMini hat schwache Antenne)
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.println("[AP] TX Power gesetzt auf 19.5 dBm (Maximum)");

  // IP-Konfiguration VOR softAP setzen
  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  Serial.println("[AP] Setze IP-Konfiguration...");
  bool configOk = WiFi.softAPConfig(apIP, gateway, subnet);
  Serial.println("[AP] WiFi.softAPConfig() = " + String(configOk ? "OK" : "FEHLER"));

  // AP starten mit Kanal 6 (oft weniger überlastet), nicht versteckt, max 4 Clients
  Serial.println("[AP] Starte softAP auf Kanal 6...");
  bool apOk = WiFi.softAP(apSSID.c_str(), AP_PASSWORD, 6, false, 4);
  Serial.println("[AP] WiFi.softAP() = " + String(apOk ? "OK" : "FEHLER"));

  delay(1000);  // Länger warten damit AP stabil wird

  apMode = true;

  // Status ausgeben
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

// Mit gespeichertem WLAN verbinden
bool connectToSavedWiFi() {
  Serial.println("[WIFI] Prüfe gespeicherte WLAN-Daten...");

  preferences.begin("wifi", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  preferences.end();

  Serial.println("[WIFI] Gespeicherte SSID: '" + ssid + "'");
  Serial.println("[WIFI] Passwort-Länge: " + String(password.length()));

  if (ssid.length() == 0) {
    Serial.println("[WIFI] Keine gespeicherten WLAN-Daten gefunden");
    return false;
  }

  Serial.println("[WIFI] Verbinde mit gespeichertem WLAN: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots % 20 == 0) {
      Serial.println();
      Serial.print("[WIFI] Status: ");
      Serial.println(WiFi.status());
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("[WIFI] ══════════════════════════════════════");
    Serial.println("[WIFI] Verbunden!");
    Serial.println("[WIFI]   SSID: " + WiFi.SSID());
    Serial.println("[WIFI]   IP: " + WiFi.localIP().toString());
    Serial.println("[WIFI]   Gateway: " + WiFi.gatewayIP().toString());
    Serial.println("[WIFI]   RSSI: " + String(WiFi.RSSI()) + " dBm");
    Serial.println("[WIFI] ══════════════════════════════════════");

    // mDNS starten
    if (MDNS.begin("litime-bms")) {
      Serial.println("[WIFI] mDNS gestartet: http://litime-bms.local");
    }

    return true;
  }

  Serial.println();
  Serial.println("[WIFI] Verbindung fehlgeschlagen! Status: " + String(WiFi.status()));
  return false;
}

void setupWebServer() {
  // Hauptseiten
  server.on("/", handleRoot);
  server.on("/bluetooth", handleBluetooth);
  server.on("/cloud", handleCloud);
  server.on("/wlan", handleWlan);

  // API Endpunkte
  server.on("/api/time", HTTP_GET, handleApiTime);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/bluetooth", HTTP_POST, handleApiBluetooth);
  server.on("/api/serial", HTTP_POST, handleApiSerial);
  server.on("/api/bms-settings", HTTP_POST, handleApiBmsSettings);
  server.on("/api/timezone", HTTP_POST, handleApiTimezone);
  server.on("/api/reset-wifi", HTTP_POST, handleApiResetWifi);
  server.on("/api/ha-settings", HTTP_POST, handleApiHaSettings);
  server.on("/api/ha-test", HTTP_POST, handleApiHaTest);

  // WLAN Konfiguration
  server.on("/scan", handleScan);
  server.on("/status", handleStatus);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/reset", HTTP_POST, handleReset);

  server.begin();
  Serial.println("Webserver gestartet");
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // Warten auf Serial

  Serial.println();
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("         LiTime LiFePO4 BMS Monitor gestartet         ");
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println();

  // NVS initialisieren (behebt "nvs_open failed: NOT_FOUND")
  Serial.println("[INIT] Initialisiere NVS...");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[INIT] NVS löschen und neu initialisieren...");
    nvs_flash_erase();
    ret = nvs_flash_init();
  }
  Serial.println("[INIT] NVS Status: " + String(ret == ESP_OK ? "OK" : "FEHLER"));

  // Chip Info ausgeben
  Serial.println("[INIT] ESP32 Chip Info:");
  Serial.println("[INIT]   Chip Model: " + String(ESP.getChipModel()));
  Serial.println("[INIT]   Chip Rev: " + String(ESP.getChipRevision()));
  Serial.println("[INIT]   CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz");
  Serial.println("[INIT]   Flash Size: " + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB");
  Serial.println("[INIT]   Free Heap: " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println();

  // Einstellungen laden
  Serial.println("[INIT] Lade Einstellungen...");
  loadSettings();
  Serial.println("[INIT] Einstellungen geladen");

  // MAC-Adresse auslesen und AP-SSID erstellen
  Serial.println("[INIT] Lese MAC-Adresse...");
  macAddress = WiFi.macAddress();
  String macSuffix = macAddress.substring(macAddress.length() - 5);
  macSuffix.replace(":", "");
  apSSID = "LiTime-BMS-" + macSuffix;

  Serial.println("[INIT] MAC: " + macAddress);
  Serial.println("[INIT] AP-SSID wird: " + apSSID);
  Serial.println();

  // WLAN verbinden oder AP starten
  Serial.println("[INIT] Starte WLAN...");
  if (!connectToSavedWiFi()) {
    Serial.println("[INIT] Kein WLAN -> starte AP");
    startAP();
  } else {
    Serial.println("[INIT] WLAN verbunden");
  }

  Serial.println();
  Serial.println("[INIT] Aktueller Modus: " + String(apMode ? "ACCESS POINT" : "STATION"));
  Serial.println();

  // Webserver starten
  Serial.println("[INIT] Starte Webserver...");
  setupWebServer();

  // NTP synchronisieren (nur wenn nicht im AP-Modus)
  if (!apMode) {
    Serial.println("[INIT] Synchronisiere NTP...");
    syncNTP();
  } else {
    Serial.println("[INIT] AP-Modus - überspringe NTP");
  }

  // BMS Verbindung wenn aktiviert
  if (bluetoothEnabled) {
    Serial.println("[INIT] Verbinde mit BMS...");
    bmsClient.init(bmsMac.c_str());
    bmsConnected = bmsClient.connect();
    if (bmsConnected) {
      Serial.println("[INIT] BMS verbunden!");
      updateBMSData();
    } else {
      Serial.println("[INIT] BMS Verbindung fehlgeschlagen!");
    }
  }

  lastBmsUpdate = millis();
  lastNtpSync = millis();

  Serial.println();
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("[INIT] Setup abgeschlossen!");
  if (apMode) {
    Serial.println("[INIT] Verbinde dich mit WLAN: " + apSSID);
    Serial.println("[INIT] Passwort: " + String(AP_PASSWORD));
    Serial.println("[INIT] Dann öffne: http://192.168.4.1");
  } else {
    Serial.println("[INIT] Webinterface: http://" + WiFi.localIP().toString());
  }
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println();
}

void loop() {
  unsigned long currentMillis = millis();

  // Webserver Anfragen verarbeiten
  server.handleClient();

  // WLAN Verbindung prüfen und bei Bedarf reconnecten (nur im STA-Modus)
  if (!apMode && (currentMillis - lastWifiCheck >= wifiCheckInterval)) {
    lastWifiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Verbindung verloren, versuche Reconnect...");
      WiFi.reconnect();

      // Kurz warten und prüfen
      unsigned long reconnectStart = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - reconnectStart < 10000) {
        delay(500);
        Serial.print(".");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WIFI] Reconnect erfolgreich! IP: " + WiFi.localIP().toString());
        // mDNS neu starten nach Reconnect
        MDNS.begin("litime-bms");
      } else {
        Serial.println("\n[WIFI] Reconnect fehlgeschlagen, versuche erneut beim nächsten Check");
      }
    }
  }

  // BMS Update (auch im AP-Modus)
  if (bluetoothEnabled && bmsConnected && (currentMillis - lastBmsUpdate >= bmsInterval * 1000)) {
    updateBMSData();
    lastBmsUpdate = currentMillis;
  }

  // BMS Reconnect wenn getrennt (auch im AP-Modus)
  if (bluetoothEnabled && !bmsConnected && (currentMillis - lastBmsUpdate >= 30000)) {
    Serial.println("[BLE] Versuche BMS Reconnect...");
    bmsConnected = bmsClient.connect();
    if (bmsConnected) {
      Serial.println("[BLE] BMS Reconnect erfolgreich!");
    } else {
      Serial.println("[BLE] BMS Reconnect fehlgeschlagen");
    }
    lastBmsUpdate = currentMillis;
  }

  // NTP Sync periodisch (nur wenn WLAN verbunden)
  if (!apMode && WiFi.status() == WL_CONNECTED && (currentMillis - lastNtpSync >= ntpSyncInterval)) {
    syncNTP();
    lastNtpSync = currentMillis;
  }

  // Home Assistant Webhook periodisch senden (nur wenn WLAN verbunden)
  if (haEnabled && !apMode && WiFi.status() == WL_CONNECTED && (currentMillis - lastHaSend >= haInterval * 1000)) {
    sendToHomeAssistant();
    lastHaSend = currentMillis;
  }
}

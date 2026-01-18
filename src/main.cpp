#include <Arduino.h>
#include <BMSClient.h>

BMSClient bmsClient;

void printBMSData() {
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("                   LiTime BMS Status                   ");
  Serial.println("══════════════════════════════════════════════════════");

  // Spannungen
  Serial.println("┌─────────────────── Spannungen ───────────────────┐");
  Serial.printf("│ Gesamtspannung:      %7.2f V                   │\n", bmsClient.getTotalVoltage());
  Serial.printf("│ Zellspannungssumme:  %7.2f V                   │\n", bmsClient.getCellVoltageSum());
  Serial.println("└──────────────────────────────────────────────────┘");

  // Einzelne Zellspannungen
  std::vector<float> cellVoltages = bmsClient.getCellVoltages();
  if (cellVoltages.size() > 0) {
    Serial.println("┌─────────────────── Zellspannungen ────────────────┐");
    for (size_t i = 0; i < cellVoltages.size(); i++) {
      Serial.printf("│ Zelle %2d:            %7.3f V                   │\n", i + 1, cellVoltages[i]);
    }
    Serial.println("└──────────────────────────────────────────────────┘");
  }

  // Strom & Kapazität
  Serial.println("┌─────────────────── Strom & Kapazität ─────────────┐");
  Serial.printf("│ Strom:               %7.2f A                   │\n", bmsClient.getCurrent());
  Serial.printf("│ SOC:                 %7d %%                   │\n", bmsClient.getSOC());
  Serial.printf("│ SOH:                 %s                         │\n", bmsClient.getSOH().c_str());
  Serial.printf("│ Verbleibend:         %7.2f Ah                  │\n", bmsClient.getRemainingAh());
  Serial.printf("│ Volle Kapazität:     %7.2f Ah                  │\n", bmsClient.getFullCapacityAh());
  Serial.println("└──────────────────────────────────────────────────┘");

  // Temperaturen
  Serial.println("┌─────────────────── Temperaturen ─────────────────┐");
  Serial.printf("│ MOSFET Temperatur:   %7d °C                  │\n", bmsClient.getMosfetTemp());
  Serial.printf("│ Zellen Temperatur:   %7d °C                  │\n", bmsClient.getCellTemp());
  Serial.println("└──────────────────────────────────────────────────┘");

  // Status
  Serial.println("┌─────────────────── Status ───────────────────────┐");
  Serial.printf("│ Batteriestatus:      %-28s│\n", bmsClient.getBatteryState().c_str());
  Serial.printf("│ Schutzstatus:        %-28s│\n", bmsClient.getProtectionState().c_str());
  Serial.printf("│ Fehlerstatus:        %-28s│\n", bmsClient.getFailureState().c_str());
  Serial.printf("│ Balancing:           %-28s│\n", bmsClient.getBalancingState().c_str());
  Serial.printf("│ Balance Memory:      %-28s│\n", bmsClient.getBalanceMemory().c_str());
  Serial.printf("│ Heizung:             %-28s│\n", bmsClient.getHeatState().c_str());
  Serial.println("└──────────────────────────────────────────────────┘");

  // Statistiken
  Serial.println("┌─────────────────── Statistiken ──────────────────┐");
  Serial.printf("│ Entladezyklen:       %7lu                     │\n", (unsigned long)bmsClient.getDischargesCount());
  Serial.printf("│ Entladene Ah:        %7.2f Ah                  │\n", bmsClient.getDischargesAhCount());
  Serial.println("└──────────────────────────────────────────────────┘");

  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("══════════════════════════════════════════════════════");
  Serial.println("         LiTime LiFePO4 BMS Monitor gestartet         ");
  Serial.println("══════════════════════════════════════════════════════");
  Serial.println();

  Serial.println("Verbinde mit BMS...");
  bmsClient.init("C8:47:80:3F:67:7C");

  if (bmsClient.connect()) {
    Serial.println("Verbunden mit BMS!");
    Serial.println();
  } else {
    Serial.println("Verbindung fehlgeschlagen!");
  }
}

void loop() {
  if (bmsClient.isConnected()) {
    bmsClient.update();
    printBMSData();
  } else {
    Serial.println("Nicht verbunden - versuche erneut...");
    bmsClient.connect();
  }
  delay(20000);
}

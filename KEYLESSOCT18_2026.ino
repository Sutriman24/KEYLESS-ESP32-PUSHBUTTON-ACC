#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_task_wdt.h> // Pustaka Hardware Watchdog Timer ESP32

// --- KONFIGURASI KEY ---
const String key1Address = "16:3a:7d:b6:f6:ce"; //smart watch
const String key2Address = "35:46:01:5c:f0:5d"; //GW62PLUS

// UUID target
const String TARGET_UUID = "25011997-2501-2501-2501-25019ABCDEF7";

// --- PIN GPIO ---
const int relayPin        = 32;
const int ledPin          = 2;
const int SeinKiri        = 25;
const int SeinKanan       = 26;
const int switchPin       = 27;
const int AccPin           = 33; // Push button ACC keyless

// --- STATUS SISTEM ---
BLEScan* pBLEScan;
bool systemUnlocked = false;
String statusInfo = "MENCARI...";
String activeMAC = "NONE";
int activeRSSI = 0;

// --- TIMER ANTI-BEGAL ---
unsigned long lastBleSeenTime = 0;
const unsigned long timeoutLimit = 15000;

// --- Tracking BLE sebelumnya ---
bool prevBleDitemukan = false;

// --- Print interval ---
unsigned long lastPrintTime = 0;
const unsigned long printInterval = 500;

// --- Scan interval ---
unsigned long lastScanTime = 0;
const unsigned long scanInterval = 1000;  // scan setiap 1 detik

// --- Logika Latch ACC berbasis Interrupt ---
volatile bool accTriggered = false;       // Menggunakan volatile karena diakses di dalam ISR Interrupt
volatile unsigned long accResetTimer = 0;
const unsigned long accResetLimit = 5000; // Memori tombol hangus jika tidak ada BLE dalam 5 detik
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;  // Mencegah pembacaan ganda saat tombol ditekan (bouncing)

// --- Fungsi Interrupt Service Routine (ISR) ---
void IRAM_ATTR accButtonInterrupt() {
  unsigned long currentTime = millis();
  // Cek apakah jeda waktu dari penekanan sebelumnya sudah melewati batas debounce
  if (currentTime - lastDebounceTime > debounceDelay) {
    if (!systemUnlocked) {
      accTriggered = true;
      accResetTimer = currentTime; // Catat waktu penekanan tombol
    }
    lastDebounceTime = currentTime;
  }
}

// --- Konfigurasi Watchdog Timer ---
const int WDT_TIMEOUT_SECONDS = 5; // Batas toleransi hang 5 detik

void setup() {
  Serial.begin(115200);
  pinMode(relayPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(SeinKiri, OUTPUT);
  pinMode(SeinKanan, OUTPUT);
  pinMode(switchPin, INPUT_PULLUP);
  
  // Set pin ACC dan pasang fungsi Interrupt saat tombol ditekan (FALLING dari HIGH ke LOW)
  pinMode(AccPin, INPUT_PULLUP); 
  attachInterrupt(digitalPinToInterrupt(AccPin), accButtonInterrupt, FALLING);

  digitalWrite(relayPin, LOW);
  digitalWrite(ledPin, LOW);
  digitalWrite(SeinKiri, LOW);
  digitalWrite(SeinKanan, LOW);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);

  // --- Inisialisasi Watchdog Timer ---
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = (1 << 0),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL);
  #else
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);
  #endif

  Serial.println(">>> SISTEM V7.1: INTERRUPT ACC PUSHBUTTON (READY) <<<");
}

void loop() {
  unsigned long now = millis();

  bool switchState = (digitalRead(switchPin) == LOW);
  bool relayAktif = (digitalRead(relayPin) == HIGH);

  // Proteksi jika tombol tidak sengaja tertekan/ditinggal tanpa ada BLE Key
  if (accTriggered && (now - accResetTimer >= accResetLimit)) {
    accTriggered = false;
    Serial.println("ACC: Waktu tunggu habis, memori tombol dikosongkan");
  }

  // === Scan BLE setiap scanInterval ===
  bool bleDitemukan = false;

  if (now - lastScanTime >= scanInterval) {
    lastScanTime = now;

    BLEScanResults foundDevices = *pBLEScan->start(1, true);

    Serial.println("--- SCAN RESULT ---");

    for (int i = 0; i < foundDevices.getCount(); i++) {
      BLEAdvertisedDevice device = foundDevices.getDevice(i);
      String addr = device.getAddress().toString().c_str();
      int rssi = device.getRSSI();
      String name = device.getName().c_str();

      Serial.printf("Device: %s | RSSI: %d | Name: %s\n", addr.c_str(), rssi, name.c_str());

      bool isKeytag = (addr == key1Address || addr == key2Address);
      bool uuidMatch = false;

      if (device.haveServiceUUID()) {
        BLEUUID advUUID = device.getServiceUUID();
        String uuidStr = advUUID.toString().c_str();
        uuidStr.toUpperCase();
        Serial.print("   Service UUID: ");
        Serial.println(uuidStr);

        if (uuidStr == TARGET_UUID) {
          uuidMatch = true;
        }
      }

      if (device.haveManufacturerData()) {
        std::string manuf = std::string(device.getManufacturerData().c_str(), device.getManufacturerData().length());
        if (manuf.length() >= 25 && (uint8_t)manuf[4] == 0x4C && (uint8_t)manuf[5] == 0x00 && (uint8_t)manuf[6] == 0x02 && (uint8_t)manuf[7] == 0x15) {
          char uuidStr[37];
          snprintf(uuidStr, sizeof(uuidStr),
                   "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                   (uint8_t)manuf[8], (uint8_t)manuf[9], (uint8_t)manuf[10], (uint8_t)manuf[11],
                   (uint8_t)manuf[12], (uint8_t)manuf[13],
                   (uint8_t)manuf[14], (uint8_t)manuf[15],
                   (uint8_t)manuf[16], (uint8_t)manuf[17],
                   (uint8_t)manuf[18], (uint8_t)manuf[19], (uint8_t)manuf[20],
                   (uint8_t)manuf[21], (uint8_t)manuf[22], (uint8_t)manuf[23]);

          String detected = String(uuidStr);
          Serial.print("   iBeacon Proximity UUID: ");
          Serial.println(detected);

          if (detected == TARGET_UUID) {
            uuidMatch = true;
          }
        }
      }

      bool detected = false;
      if (isKeytag && rssi > -75) {
        detected = true;
      } else if (uuidMatch && rssi > -75) {
        detected = true;
      }

      if (detected) {
        bleDitemukan = true;
        activeMAC = addr;
        activeRSSI = rssi;
        lastBleSeenTime = now;
        break;
      }
    }

    Serial.println("--- END SCAN ---");
    pBLEScan->clearResults();
  }

  // Logic unlock/lock dengan validasi memori tombol ACC (accTriggered)
  if (bleDitemukan && accTriggered && !systemUnlocked) {
    systemUnlocked = true;
    accTriggered = false; // Reset status tombol setelah berhasil unlock
    statusInfo = "KUNCI TERDETEKSI";
    kedipkanSein(1, 1000, 500);
    delay(500);
  }

  unsigned long timeElapsed = now - lastBleSeenTime;

  if (switchState) {
    if (timeElapsed < timeoutLimit && (systemUnlocked || relayAktif)) {
      delay(500);
      digitalWrite(relayPin, HIGH);
      digitalWrite(ledPin, HIGH);
      statusInfo = "MESIN MENYALA";
    } else {
      digitalWrite(relayPin, LOW);
      digitalWrite(ledPin, LOW);
      systemUnlocked = false;
      statusInfo = "LOCKED / TIMEOUT";
    }
  } else {
    if (!bleDitemukan && systemUnlocked) {
      digitalWrite(relayPin, LOW);
      digitalWrite(ledPin, LOW);
      delay(1000);
      kedipkanSein(2, 500, 200);
      systemUnlocked = false;
    }

    if (!switchState) {
      digitalWrite(relayPin, LOW);
      digitalWrite(ledPin, LOW);
    }

    statusInfo = bleDitemukan ? "STANDBY (READY)" : "STANDBY (LOCKED)";
  }

  // Print smooth
  if (now - lastPrintTime >= printInterval) {
    lastPrintTime = now;

    Serial.println("=================SMARTKEYLESS ESP32=================");
    Serial.printf(" STATUS SISTEM       : %s\n", statusInfo.c_str());
    Serial.printf(" BLE KEY             : %s\n", bleDitemukan ? "CONNECTED" : "DISCONNECTED");
    Serial.printf(" RSSI AKTIF          : %d dBm (Min: -75)\n", activeRSSI);
    Serial.printf(" DETIK TANPA KUNCI   : %lu / 15 Detik\n", (timeElapsed / 1000));
    Serial.println("----------------------------------------------------");
    Serial.printf(" SAKLAR (GPIO 27)    : %s\n", switchState ? "ON" : "OFF");
    Serial.printf(" RELAY (GPIO 32)     : %s\n", relayAktif ? "ON" : "OFF");
    Serial.println("====================================================");
  }

  prevBleDitemukan = bleDitemukan;
  delay(50);  

  // --- Reset/Feed Watchdog ---
  esp_task_wdt_reset(); 
}

void kedipkanSein(int jumlah, int durasiNyala, int durasiMati) {
  for (int i = 0; i < jumlah; i++) {
    digitalWrite(SeinKiri, HIGH);
    digitalWrite(SeinKanan, HIGH);
    delay(durasiNyala);
    digitalWrite(SeinKiri, LOW);
    digitalWrite(SeinKanan, LOW);
    if (i < jumlah - 1) delay(durasiMati);
  }
}

#include <SPI.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <Update.h>
#define CONFIG_BT_NIMBLE_EXT_ADV 1
#include <NimBLEDevice.h>
#if !CONFIG_BT_NIMBLE_EXT_ADV
#error Must enable extended advertising, see nimconfig.h file.
#endif
#include <CRC32.h>
#include <ArduinoJson.h>

#include "ratelimit.h"

String scannerMac;
int scannerIndex = 0;
int scannerCount = 1;

const char *ssid = "BLEAKEST01"; // SSID Name
const char *password = "";       // SSID Password - Set to NULL to have an open AP
// WiFi Channels 1, 6, and 11 have the least amount of overlap with BLE advertisement channels
const int channel = 1;        // WiFi Channel number between 1 and 13
const bool hide_SSID = false; // To disable SSID broadcast -> SSID will not appear in a basic WiFi scan
const int max_connection = 1; // Maximum simultaneous connected clients on the AP

WebServer server(80);

const char *serverIndex =
    "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
    "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
    "<input type='file' name='update'>"
    "<input type='submit' value='Update'>"
    "</form>"
    "<div id='prg'>progress: 0%</div>"
    "<script>"
    "$('form').submit(function(e){"
    "e.preventDefault();"
    "var form = $('#upload_form')[0];"
    "var data = new FormData(form);"
    " $.ajax({"
    "url: '/update',"
    "type: 'POST',"
    "data: data,"
    "contentType: false,"
    "processData:false,"
    "xhr: function() {"
    "var xhr = new window.XMLHttpRequest();"
    "xhr.upload.addEventListener('progress', function(evt) {"
    "if (evt.lengthComputable) {"
    "var per = evt.loaded / evt.total;"
    "$('#prg').html('progress: ' + Math.round(per*100) + '%');"
    "}"
    "}, false);"
    "return xhr;"
    "},"
    "success:function(d, s) {"
    "console.log('success!')"
    "},"
    "error: function (a, b, c) {"
    "}"
    "});"
    "});"
    "</script>";

// BLE Scanner vars
// FIX: volatile ensures the compiler doesn't optimize away cross-context reads
static volatile bool doConnect = false;
static uint32_t scanTime = 0; /** 0 = scan forever */

// JSON doc for logging (ArduinoJson v6 — incompatible with v7)
DynamicJsonDocument parentDoc(5120);
JsonObject logDoc = parentDoc.createNestedObject("logs");

// Bluetooth

static NimBLEScan::Phy scanPhy = NimBLEScan::Phy::SCAN_ALL;

// FIX: store address copy, not raw pointer.
// With setMaxResults(0), the NimBLEAdvertisedDevice* passed to onResult is
// only valid for the duration of the callback — storing it is a dangling pointer.
static NimBLEAddress advDeviceAddress;

class ScanCallbacks : public NimBLEScanCallbacks
{
  // FIX: signature must match the virtual in NimBLEScanCallbacks exactly.
  // The original non-const version does NOT override the virtual and the
  // callback was silently never called.
  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override
  {
    // LED ON
    digitalWrite(LED_BUILTIN, HIGH);
    // Convert device advertisement to a rateLimitId
    uint32_t id = getRateLimitId(advertisedDevice);
    // Use rateLimitId and our position in the mesh to determine
    // if the remote device is "ours" to log its advertisement data.
    // This prevents N devices logging the same advertisement data to the
    // server and SD card (duplicates)
    if (getOwnership(id, scannerIndex, scannerCount))
    {
      JsonObject scanObj = logDoc.createNestedObject(advertisedDevice->getAddress().toString());
      scanObj["name"] = NimBLEUtils::dataToHexString(
          (uint8_t *)advertisedDevice->getName().data(),
          advertisedDevice->getName().length());
      scanObj["rssi"] = advertisedDevice->getRSSI();
      scanObj["man"] = NimBLEUtils::dataToHexString(
          (uint8_t *)advertisedDevice->getManufacturerData().data(),
          advertisedDevice->getManufacturerData().length());
      scanObj["connectable"] = advertisedDevice->isConnectable();
      scanObj["addr_type"] = advertisedDevice->getAddressType();

      // Apple {0x4c, 0x00} is EVERYWHERE — skip them and prioritize anything else
      char apl[2] = {0x4c, 0x00};
      if (advertisedDevice->getManufacturerData().length() == 0 || memcmp((uint8_t *)advertisedDevice->getManufacturerData().data(), apl, 2) != 0)
      {
        // Using the rateLimitId, check if the device is in our rate limit list
        // and if the rate limit has expired
        if (!doConnect && isConnectionAllowed(id) && advertisedDevice->isConnectable())
        {
          // FIX: copy the address out; the advertisedDevice pointer is only
          // valid for the duration of this callback when setMaxResults(0)
          advDeviceAddress = advertisedDevice->getAddress();
          doConnect = true;
        }
      }
    }
    // LED OFF
    digitalWrite(LED_BUILTIN, LOW);
  }
} scanCallbacks;

// Creates/Re-Uses Client, Connects, Walks GATT tree, reads, and leaves
bool connectToServer()
{
  NimBLEClient *pClient = nullptr;

  // Try to reuse a client that already has cached services for this device
  pClient = NimBLEDevice::getClientByPeerAddress(advDeviceAddress);
  if (pClient)
  {
    // false = keep cached attributes, saves service discovery time
    if (!pClient->connect(advDeviceAddress, false))
    {
      return false;
    }
  }
  else
  {
    // Reuse a disconnected client if available, otherwise create one
    pClient = NimBLEDevice::getDisconnectedClient();
    if (!pClient)
    {
      if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS)
      {
        Serial.println("Max clients reached - no more connections available");
        return false;
      }
      pClient = NimBLEDevice::createClient();
    }

    /** Set initial connection parameters: 15ms interval, 0 latency, 510ms timeout.
     *  These settings are safe for 3 clients to connect reliably.
     *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
     */
    pClient->setConnectionParams(12, 12, 0, 51);
    pClient->setConnectTimeout(3);

    if (!pClient->connect(advDeviceAddress))
    {
      NimBLEDevice::deleteClient(pClient);
      return false;
    }
  }

  if (!pClient->isConnected())
  {
    return false;
  }

  NimBLERemoteService *pSvc = nullptr;
  NimBLERemoteCharacteristic *pChr = nullptr;

  JsonArray devTree = logDoc[pClient->getPeerAddress().toString().c_str()].createNestedArray("tree");

  const std::vector<NimBLERemoteService *> pSvcs = pClient->getServices(true);

  // Iterate over services
  for (auto sit = pSvcs.begin(); sit != pSvcs.end(); ++sit)
  {
    pSvc = *sit;
    if (pSvc)
    {
      const std::vector<NimBLERemoteCharacteristic *> pChrs = pSvc->getCharacteristics(true);

      for (auto cit = pChrs.begin(); cit != pChrs.end(); ++cit)
      {
        pChr = *cit;
        if (pChr)
        {
          JsonObject nested = devTree.createNestedObject();
          nested["svc"] = pSvc->getUUID().toString();
          nested["chr"] = pChr->getUUID().toString();

          uint8_t charProp = 0x00;
          // Speed matters — assume you're traveling in a car at 70mph
          // and another car going the opposite direction at 70mph passes you
          if (pChr->canRead())
          {
            charProp = charProp | BLE_GATT_CHR_PROP_READ;
            NimBLEAttValue rv = pChr->readValue();
            nested["val"] = NimBLEUtils::dataToHexString((uint8_t *)rv.data(), rv.length());
          }
          if (pChr->canBroadcast())
            charProp = charProp | BLE_GATT_CHR_PROP_BROADCAST;
          if (pChr->canIndicate())
            charProp = charProp | BLE_GATT_CHR_PROP_INDICATE;
          if (pChr->canNotify())
            charProp = charProp | BLE_GATT_CHR_PROP_NOTIFY;
          if (pChr->canWrite())
            charProp = charProp | BLE_GATT_CHR_PROP_WRITE;
          if (pChr->canWriteNoResponse())
            charProp = charProp | BLE_GATT_CHR_PROP_WRITE_NO_RSP;

          nested["prop"] = charProp;
        }
      }
    }
  }

  // Disconnect cleanly — call once and wait
  if (pClient->isConnected())
  {
    pClient->disconnect();
    unsigned long timeout = millis() + 3000;
    while (pClient->isConnected() && millis() < timeout)
    {
      delay(10);
    }
    NimBLEDevice::deleteClient(pClient);
  }

  return true;
}

// Configure BLE stack from scratch, set callbacks, and start an infinite scan
void setupBLE()
{
  NimBLEDevice::init("");

  // FIX: NimBLE 2.x uses int8_t dBm directly; ESP_PWR_LVL_P9 is an enum
  // value (not dBm) and would silently set the wrong power level
  NimBLEDevice::setPower(9); /** +9 dBm */

  NimBLEScan *pScan = NimBLEDevice::getScan();

  // Disabled to get all manufacturer data variants across advertisements
  pScan->setDuplicateFilter(false);

  pScan->setScanCallbacks(&scanCallbacks);

  // Tuned interval/window for faster scanning while leaving radio time for WiFi
  pScan->setInterval(45);
  pScan->setWindow(15);
  // Don't store scan results in memory — callbacks only
  pScan->setMaxResults(0);

  // Active scan probes devices for 31 bytes of manufacturer data pre-connection
  pScan->setActiveScan(true);

  pScan->setPhy(scanPhy);

  pScan->start(scanTime, false, false);
}

void disableBLEScanning()
{
  // Stop scanning before connecting — GAP events misbehave when both are active
  if (NimBLEDevice::getScan()->isScanning())
  {
    NimBLEDevice::getScan()->stop();
    while (NimBLEDevice::getScan()->isScanning())
    {
      delay(1);
    }
  }
}

// FIX: volatile ensures compiler doesn't cache the value across server.handleClient() calls
static volatile bool syncedLogs = false;

void handlePost()
{
  disableBLEScanning();
  String json = server.arg("plain");
  Serial.println(json);
  DynamicJsonDocument scannerInfo(256);
  if (DeserializationError::Ok == deserializeJson(scannerInfo, json))
  {
    // Count and index may change as scanners come online/offline
    scannerIndex = scannerInfo["si"];
    int sc = scannerInfo["ss"];
    if (sc != 0)
    {
      scannerCount = sc;
    }

    String resp;
    serializeJson(parentDoc, resp);
    server.send(200, "application/json", resp);
    syncedLogs = true;
  }
  else
  {
    server.send(400, "text/plain", "Error");
  }
}

void resetLogDoc()
{
  parentDoc.clear();
  parentDoc.garbageCollect();
  parentDoc["mac"] = scannerMac;
  logDoc = parentDoc.createNestedObject("logs");
}

void setup()
{
  Serial.begin(115200);

  delay(5000);
  Serial.println("Starting...");

  pinMode(LED_BUILTIN, OUTPUT);

  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);

  if (!WiFi.softAP(ssid, password, channel, hide_SSID, max_connection))
  {
    log_e("Soft AP creation failed.");
    while (1)
      ;
  }
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // FIX: populate scannerMac from the actual softAP MAC address
  scannerMac = WiFi.softAPmacAddress();
  Serial.print("Scanner MAC: ");
  Serial.println(scannerMac);

  // Register OTA Update routes
  server.on("/serverIndex", HTTP_GET, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex); });
  server.on("/update", HTTP_POST, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart(); }, []()
            {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    } });
  server.on("/logger", HTTP_POST, handlePost);
  server.begin();

  resetLogDoc();

  // Wait for first registration from the logger before starting to scan
  while (!syncedLogs)
  {
    server.handleClient();
  }
  syncedLogs = false;

  setupBLE();
}

void loop()
{
  while (true)
  {
    if (doConnect)
    {
      digitalWrite(LED_BUILTIN, HIGH);
      disableBLEScanning();
      Serial.print("Attempting BTLE connection...");
      doConnect = false;
      if (connectToServer())
      {
        Serial.println("done.");
        // Wait for the logger to collect the GATT tree before clearing the doc.
        // Clearing while the logger is mid-read would clobber the tree entries.
        while (!syncedLogs)
        {
          server.handleClient();
        }
        // FIX: reset immediately here so the scan restarts with a clean doc,
        // not deferred to the next loop() iteration (which would accumulate duplicates)
        syncedLogs = false;
        resetLogDoc();
      }
      else
      {
        Serial.println("fail.");
      }

      advDeviceAddress = NimBLEAddress{};
      digitalWrite(LED_BUILTIN, LOW);
      break;
    }

    server.handleClient();

    if (syncedLogs)
    {
      syncedLogs = false;
      resetLogDoc();
      break;
    }
  }

  Serial.printf_P(PSTR("free heap memory: %d\n"), ESP.getFreeHeap());
  if (!NimBLEDevice::getScan()->isScanning())
  {
    NimBLEDevice::getScan()->clearResults();
    NimBLEDevice::getScan()->start(scanTime, false, false);
  }
}

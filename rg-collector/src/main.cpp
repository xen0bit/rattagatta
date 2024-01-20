#include <SPI.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#define CONFIG_BT_NIMBLE_EXT_ADV 1
#include <NimBLEDevice.h>
#include <CRC32.h>
#include <ArduinoJson.h>

#include "ratelimit.h"

String scannerMac;
int scannerIndex = 0;
int scannerCount = 1;

const char *ssid = "BLEAKEST"; // SSID Name
const char *password = "";     // SSID Password - Set to NULL to have an open AP
// WiFi Channels 1, 6, and 11 have the least amount of overlap with BLE advertisement channels
const int channel = 1;        // WiFi Channel number between 1 and 13
const bool hide_SSID = false; // To disable SSID broadcast -> SSID will not appear in a basic WiFi scan
const int max_connection = 1; // Maximum simultaneous connected clients on the AP
WebServer server(80);

// BLE Scanner vars
static bool doConnect = false;
static uint32_t scanTime = 0; /** 0 = scan forever */

// JSON doc for logging
DynamicJsonDocument parentDoc(5120);
JsonObject logDoc = parentDoc.createNestedObject("logs");

// Bluetooth

NimBLEAdvertisedDevice *advDevice;

/** Define a class to handle the callbacks when advertisments are received */
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  void onResult(NimBLEAdvertisedDevice *advertisedDevice)
  {
    // LED ON
    digitalWrite(LED_BUILTIN, HIGH);
    // Convert device advertisement to a rateLimitId
    uint32_t id = getRateLimitId(advertisedDevice);
    // Use rateLimitId and our position in the mesh to determine
    // if the remote devices is "ours" to log it's advertisement data.
    // This prevents 15x devices logging the same advertisement data to the
    // server and it's SD card (duplicates)
    if (getOwnership(id, scannerIndex, scannerCount))
    {
      JsonObject scanObj = logDoc.createNestedObject(advertisedDevice->getAddress().toString());
      scanObj["name"] = NimBLEUtils::buildHexData(nullptr,
                                                  (uint8_t *)advertisedDevice->getName().data(),
                                                  advertisedDevice->getName().length());
      scanObj["rssi"] = advertisedDevice->getRSSI();
      scanObj["man"] = NimBLEUtils::buildHexData(nullptr,
                                                 (uint8_t *)advertisedDevice->getManufacturerData().data(),
                                                 advertisedDevice->getManufacturerData().length());
      scanObj["connectable"] = advertisedDevice->isConnectable();
      scanObj["addr_type"] = advertisedDevice->getAddressType();

      // Using the rateLimitId for a device advertisement, check if
      // the rateLimitId is populated in our rateLimitList and/or
      // if it's rateLimit has expired yet
      if (advDevice == NULL && isConnectionAllowed(id))
      {
        // Set device reference for upcoming connection attempt
        advDevice = advertisedDevice;
        // Set flag that allows early-exiting the main loop()
        doConnect = true;
      }
    }
    // LED OFF
    digitalWrite(LED_BUILTIN, LOW);
  }
};

// Creates/Re-Uses Client, Connects, Walks GATT tree, reads, and leaves
bool connectToServer()
{
  NimBLEClient *pClient = nullptr;

  /** Check if we have a client we should reuse first **/
  if (NimBLEDevice::getClientListSize())
  {
    /** Special case when we already know this device, we send false as the
     *  second argument in connect() to prevent refreshing the service database.
     *  This saves considerable time and power.
     */
    pClient = NimBLEDevice::getClientByPeerAddress(advDevice->getAddress());
    if (pClient)
    {
      if (!pClient->connect(advDevice, false))
      {
        return false;
      }
    }
    /** We don't already have a client that knows this device,
     *  we will check for a client that is disconnected that we can use.
     */
    else
    {
      pClient = NimBLEDevice::getDisconnectedClient();
    }
  }

  /** No client to reuse? Create a new one. */
  if (!pClient)
  {
    if (NimBLEDevice::getClientListSize() >= NIMBLE_MAX_CONNECTIONS)
    {
      Serial.println("Max clients reached - no more connections available");
      return false;
    }

    pClient = NimBLEDevice::createClient();

    // pClient->setClientCallbacks(&clientCB, false);

    /** Set initial connection parameters: These settings are 15ms interval, 0 latency, 120ms timout.
     *  These settings are safe for 3 clients to connect reliably, can go faster if you have less
     *  connections. Timeout should be a multiple of the interval, minimum is 100ms.
     *  Min interval: 12 * 1.25ms = 15, Max interval: 12 * 1.25ms = 15, 0 latency, 51 * 10ms = 510ms timeout
     */
    pClient->setConnectionParams(12, 12, 0, 51);
    /** Set how long we are willing to wait for the connection to complete (seconds), default is 30. */
    pClient->setConnectTimeout(3);

    if (!pClient->connect(advDevice))
    {
      /** Created a client but failed to connect, don't need to keep it as it has no data */
      NimBLEDevice::deleteClient(pClient);
      return false;
    }
  }

  if (!pClient->isConnected())
  {
    if (!pClient->connect(advDevice))
    {
      return false;
    }
  }

  /** Now we can read/write/subscribe the charateristics of the services we are interested in */
  NimBLERemoteService *pSvc = nullptr;
  NimBLERemoteCharacteristic *pChr = nullptr;
  NimBLERemoteDescriptor *pDsc = nullptr;

  if (pClient->isConnected())
  {

    JsonArray devTree = logDoc[pClient->getPeerAddress().toString().c_str()].createNestedArray("tree");

    std::vector<NimBLERemoteService *> *pSvcs = pClient->getServices(true);
    std::vector<NimBLERemoteService *>::iterator sit;

    // Iterate over services
    for (sit = pSvcs->begin(); sit < pSvcs->end(); sit++)
    {
      pSvc = *sit;
      if (pSvc)
      {
        // Iterate over characteristics
        std::vector<NimBLERemoteCharacteristic *> *pChrs = pSvc->getCharacteristics(true);
        std::vector<NimBLERemoteCharacteristic *>::iterator cit;

        for (cit = pChrs->begin(); cit < pChrs->end(); cit++)
        {
          pChr = *cit;
          if (pChr)
          { /** make sure it's not null */
            JsonObject nested = devTree.createNestedObject();
            nested["svc"] = pSvc->getUUID().to128().toString();
            nested["chr"] = pChr->getUUID().to128().toString();

            uint8_t charProp = 0x00;
            // Speed matters, assume you're traveling in a car at 70mph
            // and another car going the opposite direction at 70mph passes you
            if (pChr->canRead())
            {
              // Set read property
              charProp = charProp | BLE_GATT_CHR_PROP_READ;
              NimBLEAttValue rv = pChr->readValue();
              nested["val"] = NimBLEUtils::buildHexData(nullptr,
                                                        (uint8_t *)rv.data(),
                                                        rv.length());
            }

            // This is bullshit, there has to be a better way and I'm sorry but it *does* work
            if (pChr->canBroadcast())
            {
              charProp = charProp | BLE_GATT_CHR_PROP_BROADCAST;
            }
            if (pChr->canIndicate())
            {
              charProp = charProp | BLE_GATT_CHR_PROP_INDICATE;
            }
            if (pChr->canNotify())
            {
              charProp = charProp | BLE_GATT_CHR_PROP_NOTIFY;
            }
            if (pChr->canWrite())
            {
              charProp = charProp | BLE_GATT_CHR_PROP_WRITE;
            }
            if (pChr->canWriteNoResponse())
            {
              charProp = charProp | BLE_GATT_CHR_PROP_WRITE_NO_RSP;
            }

            nested["prop"] = charProp;
          }
        }
      }
    }
  }

  // Cleanup
  if (pClient->isConnected())
  {
    pClient->disconnect();
    while (pClient->isConnected())
    {
      delay(50);
      pClient->disconnect();
    }
    NimBLEDevice::deleteClient(pClient);
  }

  return true;
}

// No-Op, leaving here in case I ever decide to use it
void scanEndedCB(NimBLEScanResults results)
{
}

// Configure BLE stack from scratch, set callbacks, and start an infinite scan
void setupBLE()
{
  // We are a client, no devicename needed
  NimBLEDevice::init("");

  // Set the transmit power, default is 3db
#ifdef ESP_PLATFORM
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
#else
  NimBLEDevice::setPower(9); /** +9db */
#endif

  // Create a scan
  NimBLEScan *pScan = NimBLEDevice::getScan();

  // Disabled, not because we want duplicate events, but because radio advertisements
  // for manufacturer data are sometimes not equally distributed. IE: devicename may
  // only show in 1/3rd of advertisements when scanning really quickly. This allows us
  // to just flatten in the JSON object.
  pScan->setDuplicateFilter(false);

  // Register them callbacks (where the magic happens!) so we don't store
  // scanResults in memory
  pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());

  // Some math was involved here, I don't remember, but it works and is faster than
  // default implementation.
  pScan->setInterval(45);
  pScan->setWindow(15);
  // Store none of the scanResult in memory (callbacks only)
  pScan->setMaxResults(0);

  //"active" probing of BLE servers to coerce them to give us that sweet sweet
  // 31 bytes of manufacturer data without even needing a connection yet
  pScan->setActiveScan(true);

  // Lets goooooooooooooooooooooooo
  pScan->start(scanTime, scanEndedCB, false);
}

void disableBLEScanning()
{
  // Ensure not scanning when connecting. BLE GAP events are strange, you get weird things if
  // you don't do this
  if (NimBLEDevice::getScan()->isScanning())
  {
    // Serial.print("Forcing scan to stop...");
    NimBLEDevice::getScan()->stop();
    while (NimBLEDevice::getScan()->isScanning())
    {
      delay(1);
    }
    // Serial.println("done");
  }
}

bool syncedLogs = false;

void handlePost()
{
  disableBLEScanning();
  String json = server.arg("plain");
  Serial.println(json);
  DynamicJsonDocument scannerInfo(256);
  if (DeserializationError::Ok == deserializeJson(scannerInfo, json))
  {
    // The count of total scanners and our position within the index may change
    // as new scanners are brought online/offline. With each logging, we update our
    // locally cached information.
    scannerIndex = scannerInfo["si"];
    int sc = scannerInfo["ss"];
    if (sc != 0)
    {
      scannerCount = sc;
    }

    // Respond with our object
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

void setup()
{
  Serial.begin(115200);

  delay(5000);
  Serial.println("Starting...");

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  // dont be silly, im still gonna send it
  // dont be silly, im still gonna send it
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);

  // Create the BLEAKEST WiFi Access Point Server
  if (!WiFi.softAP(ssid, password, channel, hide_SSID, max_connection))
  {
    log_e("Soft AP creation failed.");
    while (1)
      ;
  }
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  server.on("/logger", HTTP_POST, handlePost);
  server.begin();

  // Construct base JSON document
  parentDoc.to<JsonObject>();
  parentDoc["mac"] = scannerMac;
  logDoc = parentDoc.createNestedObject("logs");

  // Forcefully wait to start scanning until we've been registered
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
      // LED ON
      digitalWrite(LED_BUILTIN, HIGH);
      // BLE scanner my be writing to JSON Document
      disableBLEScanning();
      Serial.print("Attempting BTLE connection...");
      // reset for next round
      doConnect = false;
      if (connectToServer())
      {
        Serial.println("done.");
      }
      else
      {
        Serial.println("fail.");
      }
      // Clear the NimBLE radio stack fully if we've attempted a connection
      // NimBLEDevice::deinit(true);

      advDevice = NULL;
      // LED OFF
      digitalWrite(LED_BUILTIN, LOW);
      break;
    }

    // Listen for a server connection
    // POST handler disables BLE scanning if triggered
    server.handleClient();

    // Check if we cleared the logs
    if (syncedLogs)
    {
      syncedLogs = false;
      // BLE scanner my be writing to JSON Document
      // disableBLEScanning();
      // Reset our JSON doc to initial state
      parentDoc.clear();
      parentDoc.garbageCollect();
      parentDoc["mac"] = scannerMac;
      logDoc = parentDoc.createNestedObject("logs");

      // NimBLEDevice::getScan()->clearResults();
      // NimBLEDevice::getScan()->start(scanTime, scanEndedCB, false);
      break;
    }
  }

  Serial.printf_P(PSTR("free heap memory: %d\n"), ESP.getFreeHeap());
  // Logs were sent to server and WiFi has been disconnected
  // Time to stand back up the BLE stack again!
  if (!NimBLEDevice::getScan()->isScanning())
  {
    NimBLEDevice::getScan()->clearResults();
    NimBLEDevice::getScan()->start(scanTime, scanEndedCB, false);
  }
  // setupBLE();
}

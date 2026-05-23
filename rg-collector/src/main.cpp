/**
 * rg-collector — BLE survey node (XIAO ESP32-S3)
 *
 * New topology: STA mode — connects to the logger's AP "BLEAKEST".
 * Registers on boot to receive scannerIndex/scannerCount, then scans
 * continuously. Logger polls /logger over the shared subnet; no WiFi
 * reconnect per poll.
 *
 * Core split (ESP32-S3 dual-core):
 *   Core 0 — NimBLE host task (onResult callbacks, GATT stack)
 *   Core 1 — Arduino loop() — HTTP server + GATT connection logic
 *
 * JsonDocument is shared across cores; all writes are guarded by docMutex.
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

#define CONFIG_BT_NIMBLE_EXT_ADV 1
#include <NimBLEDevice.h>
#if !CONFIG_BT_NIMBLE_EXT_ADV
#error Must enable extended advertising — see nimconfig.h
#endif

#include <CRC32.h>
#include <ArduinoJson.h>
#include "ratelimit.h"

// ---------------------------------------------------------------------------
// Config — logger's AP that all collectors join
// ---------------------------------------------------------------------------
static const char *LOGGER_SSID = "BLEAKEST";
static const char *LOGGER_PASS = "prevent_stray_clients";
static const IPAddress LOGGER_IP(192, 168, 4, 1);

// Must match COLLECTOR_FW_VERSION in rg-logger/src/main.cpp.
// Increment both together when deploying new collector firmware.
#define COLLECTOR_FW_VERSION 1

// ---------------------------------------------------------------------------
// Identity — persisted across reboots via NVS
// ---------------------------------------------------------------------------
static int scannerIndex = 0;
static int scannerCount = 1;
static int lastLoggerFwVersion = 0;
static String scannerMac;

static void loadConfig()
{
  Preferences p;
  p.begin("rg", true);
  scannerIndex = p.getInt("idx", 0);
  scannerCount = p.getInt("cnt", 1);
  p.end();
}

static void saveConfig(int idx, int cnt)
{
  Preferences p;
  p.begin("rg", false);
  p.putInt("idx", idx);
  p.putInt("cnt", cnt);
  p.end();
}

// ---------------------------------------------------------------------------
// JSON log document — shared between NimBLE (core 0) and loop/HTTP (core 1)
// ---------------------------------------------------------------------------
static JsonDocument parentDoc;
static JsonObject logDoc;
static SemaphoreHandle_t docMutex;

static void initLogDoc()
{
  // Caller must hold docMutex (or call before tasks start)
  parentDoc.clear();
  parentDoc.shrinkToFit();
  parentDoc["mac"] = scannerMac;
  logDoc = parentDoc["logs"].to<JsonObject>();
}

// ---------------------------------------------------------------------------
// BLE state
// ---------------------------------------------------------------------------
static volatile bool doConnect = false;
static NimBLEAddress advDeviceAddress;
static const uint32_t SCAN_TIME = 0; // 0 = forever
static NimBLEScan::Phy scanPhy = NimBLEScan::Phy::SCAN_ALL;

// ---------------------------------------------------------------------------
// Scan callbacks (runs on NimBLE host task, core 0)
// ---------------------------------------------------------------------------
class ScanCallbacks : public NimBLEScanCallbacks
{
  void onResult(const NimBLEAdvertisedDevice *dev) override
  {
    // Hard heap guard — skip rather than crash
    if (ESP.getFreeHeap() < 40000)
      return;

    uint32_t id = getRateLimitId(dev);
    if (!getOwnership(id, scannerIndex, scannerCount))
      return;

    // Short mutex timeout — never block the NimBLE stack
    if (xSemaphoreTake(docMutex, pdMS_TO_TICKS(5)) != pdTRUE)
      return;

    JsonObject scanObj = logDoc[dev->getAddress().toString()].to<JsonObject>();
    scanObj["name"] = NimBLEUtils::dataToHexString(
        (uint8_t *)dev->getName().data(), dev->getName().length());
    scanObj["rssi"] = dev->getRSSI();
    scanObj["man"] = NimBLEUtils::dataToHexString(
        (uint8_t *)dev->getManufacturerData().data(), dev->getManufacturerData().length());
    scanObj["connectable"] = dev->isConnectable();
    scanObj["addr_type"] = dev->getAddressType();

    // Apple {0x4c,0x00} saturates the airwaves — skip for connection attempts
    const char apl[2] = {0x4c, 0x00};
    bool isApple = dev->getManufacturerData().length() >= 2 &&
                   memcmp(dev->getManufacturerData().data(), apl, 2) == 0;

    if (!isApple && !doConnect && isConnectionAllowed(id) && dev->isConnectable())
    {
      advDeviceAddress = dev->getAddress();
      doConnect = true;
    }

    xSemaphoreGive(docMutex);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
} scanCallbacks;

// ---------------------------------------------------------------------------
// BLE helpers
// ---------------------------------------------------------------------------
static void disableBLEScanning()
{
  if (NimBLEDevice::getScan()->isScanning())
  {
    NimBLEDevice::getScan()->stop();
    while (NimBLEDevice::getScan()->isScanning())
      delay(1);
  }
}

static void restartBLEScan()
{
  if (!NimBLEDevice::getScan()->isScanning())
  {
    NimBLEDevice::getScan()->clearResults();
    NimBLEDevice::getScan()->start(SCAN_TIME, false, false);
  }
}

// Walk GATT tree, write results into logDoc under docMutex.
// Scan must already be stopped before calling.
static bool connectToServer()
{
  NimBLEClient *pClient = NimBLEDevice::getClientByPeerAddress(advDeviceAddress);
  if (pClient)
  {
    if (!pClient->connect(advDeviceAddress, false))
      return false;
  }
  else
  {
    pClient = NimBLEDevice::getDisconnectedClient();
    if (!pClient)
    {
      if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS)
      {
        Serial.println("Max clients reached");
        return false;
      }
      pClient = NimBLEDevice::createClient();
    }
    pClient->setConnectionParams(12, 12, 0, 51);
    pClient->setConnectTimeout(3);
    if (!pClient->connect(advDeviceAddress))
    {
      NimBLEDevice::deleteClient(pClient);
      return false;
    }
  }

  if (!pClient->isConnected())
    return false;

  // Scan is stopped, so no concurrent onResult writes — mutex is still correct
  // for the HTTP handler which could serialize the doc at any time.
  if (xSemaphoreTake(docMutex, pdMS_TO_TICKS(5000)) != pdTRUE)
  {
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    return false;
  }

  JsonArray devTree =
      logDoc[pClient->getPeerAddress().toString().c_str()]["tree"].to<JsonArray>();

  const auto &svcs = pClient->getServices(true);
  for (auto *svc : svcs)
  {
    if (!svc)
      continue;
    for (auto *chr : svc->getCharacteristics(true))
    {
      if (!chr)
        continue;
      JsonObject node = devTree.add<JsonObject>();
      node["svc"] = svc->getUUID().toString();
      node["chr"] = chr->getUUID().toString();

      uint8_t prop = 0;
      if (chr->canRead())
      {
        prop |= BLE_GATT_CHR_PROP_READ;
        NimBLEAttValue rv = chr->readValue();
        node["val"] = NimBLEUtils::dataToHexString((uint8_t *)rv.data(), rv.length());
      }
      if (chr->canBroadcast())
        prop |= BLE_GATT_CHR_PROP_BROADCAST;
      if (chr->canIndicate())
        prop |= BLE_GATT_CHR_PROP_INDICATE;
      if (chr->canNotify())
        prop |= BLE_GATT_CHR_PROP_NOTIFY;
      if (chr->canWrite())
        prop |= BLE_GATT_CHR_PROP_WRITE;
      if (chr->canWriteNoResponse())
        prop |= BLE_GATT_CHR_PROP_WRITE_NO_RSP;
      node["prop"] = prop;
    }
  }

  xSemaphoreGive(docMutex);

  pClient->disconnect();
  unsigned long t = millis() + 3000;
  while (pClient->isConnected() && millis() < t)
    delay(10);
  NimBLEDevice::deleteClient(pClient);
  return true;
}

static void setupBLE()
{
  NimBLEDevice::init("");
  NimBLEDevice::setPower(9); // +9 dBm

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setDuplicateFilter(false);
  scan->setScanCallbacks(&scanCallbacks);
  scan->setInterval(45);
  scan->setWindow(15);
  scan->setMaxResults(0);
  scan->setActiveScan(true);
  scan->setPhy(scanPhy);
  scan->start(SCAN_TIME, false, false);
}

// ---------------------------------------------------------------------------
// WiFi + registration
// ---------------------------------------------------------------------------
static bool connectToLoggerAP()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(LOGGER_SSID, LOGGER_PASS);
  unsigned long t = millis() + 20000;
  while (WiFi.status() != WL_CONNECTED && millis() < t)
    delay(200);
  return WiFi.status() == WL_CONNECTED;
}

// Register with the logger and receive assigned scannerIndex/scannerCount.
// Returns true on success; the logger response updates config + NVS if changed.
static bool registerWithLogger()
{
  WiFiClient wc;
  HTTPClient http;
  String url = "http://" + LOGGER_IP.toString() + "/register";
  if (!http.begin(wc, url))
    return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  JsonDocument req;
  req["mac"] = scannerMac;
  req["idx"] = scannerIndex; // hint: previously stored index
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code == 200)
  {
    JsonDocument resp;
    if (deserializeJson(resp, http.getString()) == DeserializationError::Ok)
    {
      int ni = resp["si"] | scannerIndex;
      int nc = resp["ss"] | scannerCount;
      lastLoggerFwVersion = resp["fw"] | 0;
      if (ni != scannerIndex || nc != scannerCount)
      {
        scannerIndex = ni;
        scannerCount = nc;
        saveConfig(ni, nc);
      }
    }
    http.end();
    return true;
  }
  http.end();
  return false;
}

// Pull firmware from logger if versions differ. Reboots on success.
static void checkAndApplyFirmwareUpdate()
{
  if (lastLoggerFwVersion == 0 || lastLoggerFwVersion == COLLECTOR_FW_VERSION)
    return;

  Serial.printf("OTA: local fw=%d logger fw=%d — updating\n",
                COLLECTOR_FW_VERSION, lastLoggerFwVersion);
  WiFiClient wc;
  String url = "http://" + LOGGER_IP.toString() + "/collector_firmware";
  t_httpUpdate_return ret = httpUpdate.update(wc, url);
  switch (ret)
  {
  case HTTP_UPDATE_OK:
    // httpUpdate reboots automatically after flashing
    break;
  case HTTP_UPDATE_FAILED:
    Serial.printf("OTA failed (%d): %s\n",
                  httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
    break;
  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("OTA: server reported no update");
    break;
  }
}

// ---------------------------------------------------------------------------
// HTTP server (served from loop(), core 1)
// ---------------------------------------------------------------------------
static WebServer server(80);

static const char *OTA_PAGE =
    "<form method='POST' action='#' enctype='multipart/form-data'>"
    "<input type='file' name='update'><input type='submit' value='Flash'>"
    "</form>";

// Logger polls here: serialize doc, clear it, respond — all under mutex.
// BLE scanning continues on core 0 during this; the mutex prevents concurrent
// JSON writes. The ESP32 radio handles WiFi/BLE coexistence at the PHY level.
static void handleLoggerPost()
{
  // Update scannerIndex/scannerCount if the logger sent new values
  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, server.arg("plain")) == DeserializationError::Ok)
  {
    int ni = reqDoc["si"] | scannerIndex;
    int nc = reqDoc["ss"] | scannerCount;
    if (nc > 0 && (ni != scannerIndex || nc != scannerCount))
    {
      scannerIndex = ni;
      scannerCount = nc;
      saveConfig(ni, nc);
    }
  }

  if (xSemaphoreTake(docMutex, pdMS_TO_TICKS(500)) != pdTRUE)
  {
    server.send(503, "text/plain", "Busy");
    return;
  }
  String resp;
  serializeJson(parentDoc, resp);
  initLogDoc(); // clear + reinit while holding the mutex
  xSemaphoreGive(docMutex);

  server.send(200, "application/json", resp);
}

static void setupHTTPServer()
{
  server.on("/serverIndex", HTTP_GET, []()
            {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", OTA_PAGE); });
  server.on("/update", HTTP_POST, []()
            {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      ESP.restart(); }, []()
            {
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START)
      {
        Serial.printf("OTA: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      }
      else if (up.status == UPLOAD_FILE_WRITE)
      {
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
          Update.printError(Serial);
      }
      else if (up.status == UPLOAD_FILE_END)
      {
        if (Update.end(true))
          Serial.printf("OTA done: %u bytes\n", up.totalSize);
        else
          Update.printError(Serial);
      } });
  server.on("/logger", HTTP_POST, handleLoggerPost);
  server.begin();
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(2000);
  Serial.println("rg-collector starting");

  pinMode(LED_BUILTIN, OUTPUT);

  docMutex = xSemaphoreCreateMutex();

  loadConfig();

  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);

  // MAC is stable before any WiFi mode is set
  scannerMac = WiFi.macAddress();
  initLogDoc();

  // Connect to logger AP — retry indefinitely (logger may still be booting)
  Serial.printf("Connecting to %s...", LOGGER_SSID);
  while (!connectToLoggerAP())
  {
    Serial.print(".");
    delay(3000);
  }
  Serial.println(" IP: " + WiFi.localIP().toString());

  // Register — get assigned scannerIndex/scannerCount from logger
  Serial.print("Registering...");
  while (!registerWithLogger())
  {
    Serial.print(".");
    delay(2000);
  }
  Serial.printf(" idx=%d cnt=%d fw=%d\n", scannerIndex, scannerCount, lastLoggerFwVersion);

  // Apply firmware update if logger carries a newer version; reboots on success.
  checkAndApplyFirmwareUpdate();

  setupHTTPServer();
  setupBLE();
}

void loop()
{
  // Reconnect if WiFi dropped (e.g. logger rebooted)
  if (WiFi.status() != WL_CONNECTED)
  {
    disableBLEScanning();
    Serial.println("WiFi lost — reconnecting");
    while (!connectToLoggerAP())
      delay(3000);
    while (!registerWithLogger())
      delay(2000);
    restartBLEScan();
  }

  server.handleClient();

  if (doConnect)
  {
    doConnect = false;
    disableBLEScanning();
    Serial.print("GATT -> ");
    if (connectToServer())
      Serial.println("ok");
    else
      Serial.println("fail");
    advDeviceAddress = NimBLEAddress{};
    restartBLEScan();
  }

  Serial.printf_P(PSTR("heap: %d\n"), ESP.getFreeHeap());
}

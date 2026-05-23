/**
 * rg-logger — M5Stack Core2 coordinator
 *
 * New topology: logger creates the "BLEAKEST" AP. Collectors connect as STAs,
 * POST to /register to receive their assigned index, then serve /logger at
 * their own IP. The logger polls each registered collector sequentially over
 * the shared 192.168.4.0/24 subnet — no WiFi reconnect per collector.
 *
 * MAC→index mapping is persisted in NVS (Preferences) so assignments survive
 * a logger reboot without collector intervention.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <M5Core2.h>
#pragma GCC diagnostic pop

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "ArduinoJson.h"
#include "health.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// AP config — collectors connect here
// ---------------------------------------------------------------------------
static const char *AP_SSID     = "BLEAKEST";
static const char *AP_PASS     = "";
static const int   AP_CHANNEL  = 1; // minimal BLE-adv-channel overlap
static const int   AP_MAX_STA  = 15;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

// LCD constants
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define RECT_W        45
#define RECT_H        60

static long totalEvents = 0;

// ---------------------------------------------------------------------------
// NVS MAC→index persistence
// ---------------------------------------------------------------------------
// Stored as "mac_N" (hex string) + "cnt" (total registered).
// Allows index assignment to survive logger reboots.

static void nvsSaveMapping()
{
  Preferences p;
  p.begin("rg-log", false);
  p.putInt("cnt", seenScanners);
  for (int i = 0; i < seenScanners; i++)
  {
    char key[8];
    snprintf(key, sizeof(key), "mac_%d", i);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             healthStatusList[i].mac[0], healthStatusList[i].mac[1],
             healthStatusList[i].mac[2], healthStatusList[i].mac[3],
             healthStatusList[i].mac[4], healthStatusList[i].mac[5]);
    p.putString(key, mac);
  }
  p.end();
}

static void nvsLoadMapping()
{
  Preferences p;
  p.begin("rg-log", true);
  int cnt = p.getInt("cnt", 0);
  for (int i = 0; i < cnt && i < MAX_HEALTH_ITEMS; i++)
  {
    char key[8];
    snprintf(key, sizeof(key), "mac_%d", i);
    String ms = p.getString(key, "");
    if (ms.length() == 17) // "xx:xx:xx:xx:xx:xx"
    {
      uint8_t mac[6];
      macStringToBytes(mac, ms);
      // IP is unknown until collector re-registers, leave as zero
      // addScannerToList records the entry with index = seenScanners
      addScannerToList(mac, IPAddress(0, 0, 0, 0));
    }
  }
  p.end();
}

// ---------------------------------------------------------------------------
// Registration HTTP endpoint
// ---------------------------------------------------------------------------
static WebServer server(80);

static void handleRegister()
{
  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, server.arg("plain")) != DeserializationError::Ok)
  {
    server.send(400, "text/plain", "Bad JSON");
    return;
  }

  String macStr = reqDoc["mac"] | String("");
  if (macStr.length() != 17)
  {
    server.send(400, "text/plain", "Invalid MAC");
    return;
  }

  uint8_t mac[6];
  macStringToBytes(mac, macStr);

  IPAddress collectorIP = server.client().remoteIP();

  int idx = getScannerIndex(mac);
  if (idx == -1)
  {
    // New collector — assign next index
    idx = addScannerToList(mac, collectorIP);
    if (idx == -1)
    {
      server.send(503, "text/plain", "Scanner list full");
      return;
    }
    nvsSaveMapping();
    Serial.printf("New scanner #%d: %s @ %s\n",
                  idx, macStr.c_str(), collectorIP.toString().c_str());
  }
  else
  {
    // Returning collector — update its IP (may have changed via DHCP)
    healthStatusList[idx].ip = collectorIP;
    Serial.printf("Re-registered #%d: %s @ %s\n",
                  idx, macStr.c_str(), collectorIP.toString().c_str());
  }

  JsonDocument resp;
  resp["si"] = idx;
  resp["ss"] = seenScanners;
  String body;
  serializeJson(resp, body);
  server.send(200, "application/json", body);
}

// ---------------------------------------------------------------------------
// LCD
// ---------------------------------------------------------------------------
static void updateLcd()
{
  const int rows = 2, cols = 7;
  for (int row = 0; row < rows; row++)
  {
    for (int col = 0; col < cols; col++)
    {
      int x  = col * RECT_W;
      int y  = row * RECT_H;
      int si = (row * cols) + col;

      if (si < seenScanners)
      {
        uint16_t color = getHealthy(si) ? TFT_GREEN : TFT_RED;
        M5.Lcd.fillRect(x, y, RECT_W, RECT_H, color);
      }
      M5.Lcd.drawRect(x, y, RECT_W, RECT_H, TFT_WHITE);
      M5.Lcd.setTextColor(TFT_WHITE);
      M5.Lcd.setTextSize(3);
      M5.Lcd.drawNumber((row * cols) + col, x + 3, y + 3);
    }
  }

  M5.Lcd.fillRect(0, RECT_H * 3, SCREEN_WIDTH, SCREEN_HEIGHT, TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE);
  M5.Lcd.setTextSize(7);
  M5.Lcd.drawNumber(totalEvents, 0, RECT_H * 3 + 5);
}

// ---------------------------------------------------------------------------
// Collector polling — single HTTP POST to collector's IP, no reconnect
// ---------------------------------------------------------------------------
static void appendLog(const String &log)
{
  File f = SD.open("/log.jsonl", FILE_APPEND);
  if (!f) { Serial.println("SD open failed"); return; }
  f.println(log);
  f.close();
  Serial.println("wrote log to SD");
}

static bool pollCollector(int idx)
{
  IPAddress ip = healthStatusList[idx].ip;
  if ((uint32_t)ip == 0) return false; // NVS-loaded entry not yet re-registered

  WiFiClient wc;
  HTTPClient http;
  String url = "http://" + ip.toString() + "/logger";
  if (!http.begin(wc, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000); // allow time for any in-progress GATT walk to finish

  JsonDocument reqDoc;
  reqDoc["si"] = idx;
  reqDoc["ss"] = seenScanners;
  String body;
  serializeJson(reqDoc, body);

  int code = http.POST(body);
  if (code == 200)
  {
    String resp = http.getString();
    JsonDocument logDoc;
    if (deserializeJson(logDoc, resp) == DeserializationError::Ok)
    {
      int n = logDoc["logs"].size();
      if (n > 0)
      {
        totalEvents += n;
        appendLog(resp);
      }
    }
    http.end();
    return true;
  }

  Serial.printf("Poll #%d failed: %d\n", idx, code);
  http.end();
  return false;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup()
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  M5.begin();
#pragma GCC diagnostic pop

  if (!SD.begin())
  {
    M5.Lcd.println("SD init failed");
    while (1) ;
  }
  M5.Lcd.println("SD OK");

  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);

  // Load previously-registered scanner MACs so they keep their indices
  // across logger reboots (collectors will re-register and refresh their IPs)
  nvsLoadMapping();

  // Create AP — collectors connect here
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
  if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, false, AP_MAX_STA))
  {
    M5.Lcd.println("AP failed");
    while (1) ;
  }
  Serial.printf("AP started: %s @ %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/register", HTTP_POST, handleRegister);
  server.begin();

  M5.Lcd.clear(TFT_BLACK);
  M5.Lcd.println("Waiting for collectors...");
}

void loop()
{
  // Handle registration requests from newly-connected collectors
  server.handleClient();

  // Poll every registered collector that has a valid IP
  for (int i = 0; i < seenScanners; i++)
  {
    if (pollCollector(i)) updateScannerHealth(i);
    updateLcd();
    // Keep handling registrations between polls so new collectors aren't missed
    server.handleClient();
  }
}

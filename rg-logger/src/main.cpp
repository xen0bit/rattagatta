#include <M5Core2.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include "ArduinoJson.h"

#include "utils.h"
#include "health.h"

// M5Stack Core2 LCD dimensions
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

char ssid[] = "BLEAKEST"; //  your network SSID (name)
char pass[] = "";         // your network password

long totalEvents = 0;

void appendLog(String log)
{
  File file = SD.open("/log.jsonl", FILE_APPEND);
  if (!file)
  {
    Serial.println(F("Failed to create file"));
    return;
  }
  file.println(log);
  Serial.println("wrote log to disk");
  file.close();
}

void updateLcd()
{
  int rows = 2;
  int cols = 7;
  int RECT_WIDTH = 45;
  int RECT_HEIGHT = 60;
  // Loop through each row
  for (int row = 0; row < rows; row++)
  {
    // Loop through each column
    for (int col = 0; col < cols; col++)
    {
      // Calculate the x and y coordinates of the top-left corner of the current rectangle
      int x = col * RECT_WIDTH;
      int y = row * RECT_HEIGHT;

      int scannerIndex = (row * cols) + col;
      if (scannerIndex < seenScanners)
      {
        bool isHealthy = getHealthy(scannerIndex);
        // Draw the rectangle on the display
        if (isHealthy)
        {
          M5.Lcd.fillRect(x, y, RECT_WIDTH, RECT_HEIGHT, TFT_GREEN);
        }
        else
        {
          M5.Lcd.fillRect(x, y, RECT_WIDTH, RECT_HEIGHT, TFT_RED);
        }
      }

      // Draw Grid on top of the color-coded rectanlges
      M5.Lcd.drawRect(x, y, RECT_WIDTH, RECT_HEIGHT, TFT_WHITE);

      // Label the grid with numbers for the scannerIndex
      long gridLabel = (row * cols) + col;
      M5.Lcd.setTextColor(TFT_WHITE);
      M5.Lcd.setTextSize(3);
      M5.Lcd.drawNumber(gridLabel, x + 3, y + 3);
    }
  }

  // Render totalEvents
  M5.Lcd.fillRect(0, (RECT_HEIGHT * 3), SCREEN_WIDTH, SCREEN_HEIGHT, TFT_BLACK);
  int xo = (RECT_WIDTH * 0);
  int yo = (RECT_HEIGHT * 3) + 5;
  M5.Lcd.setTextColor(TFT_PURPLE);
  M5.Lcd.setTextSize(5);
  M5.Lcd.drawNumber(totalEvents, xo, yo);
}

void sweepForBleakest()
{
  int n = WiFi.scanNetworks();
  if (n != 0)
  {
    for (int i = 0; i < n; ++i)
    {
      if (WiFi.SSID(i) == "BLEAKEST")
      {
        uint8_t mac[6];
        memcpy(mac, WiFi.BSSID(i), 6);

        if (!isScannerInList(mac))
        {
          Serial.print("Adding ");
          Serial.println(WiFi.BSSIDstr(i));
          addScannerToList(mac, WiFi.channel(i));
        }
      }
    }
  }
}

bool connectWiFi(int scannerIndex)
{
  Serial.print("Connecting to wifi...");
  WiFi.begin(ssid, pass, healthStatusList[scannerIndex].channel, healthStatusList[scannerIndex].mac);
  // 10s timeout
  unsigned long timeout = millis() + 10000;
  while (WiFi.status() != WL_CONNECTED && millis() < timeout)
  {
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("done.");
    return true;
  }
  else
  {
    Serial.println("fail.");
    return false;
  }
}

// Disconnects from GW-00 SSID. If already disconnected, this makes no changes.
void disconnectWiFi()
{
  Serial.println("Disconnecting from wifi...");
  WiFi.disconnect();
  while (WiFi.status() != WL_DISCONNECTED)
  {
    delay(50);
  }
  Serial.println("done.");
}

bool getLogJson(int scannerIndex)
{
  WiFiClient client;
  HTTPClient http;
  int respCode = 0;

  Serial.print("\nStarting TCP connection...");
  if (client.connect(WiFi.gatewayIP(), 80))
  {
    Serial.println("connected...");
    if (client.connected())
    {
      // Serialize JSON document to string
      String json;
      DynamicJsonDocument registerScanner(256);
      registerScanner["si"] = scannerIndex;
      registerScanner["ss"] = seenScanners;
      serializeJson(registerScanner, json);
      // Serial.println(json);

      // Construct and send POST
      String endpoint = "http://" + WiFi.gatewayIP().toString() + "/logger";
      http.begin(client, endpoint);
      http.addHeader("Content-Type", "application/json");
      respCode = http.POST(json);

      // Success!
      if (respCode == 200)
      {
        // Read response into a JSON Doc
        DynamicJsonDocument logDoc(5120);
        String resp = http.getString();
        if (DeserializationError::Ok == deserializeJson(logDoc, resp))
        {
          Serial.println("Deserialize OK!");
          totalEvents += logDoc["logs"].size();
          // serializeJsonPretty(logDoc, Serial);
          // Serial.println(logDoc["logs"].size());
          appendLog(resp);
        }
      }
      // Failure :(
      else
      {
        Serial.print("failed to POST ");
        Serial.print(respCode);
        Serial.println("...");
      }

      // Disconnect
      http.end();
    }
  }

  if (respCode == 200)
  {
    return true;
  }
  else
  {
    return false;
  }
}

void setup()
{
  M5.begin();
  if (!SD.begin())
  { // Initialize the SD card. 初始化SD卡
    M5.Lcd.println(
        "Card failed, or not present"); // Print a message if the SD card
                                        // initialization fails or if the
                                        // SD card does not exist
    // 如果SD卡初始化失败或者SD卡不存在，则打印消息
    while (1)
      ;
  }
  M5.Lcd.println("TF card initialized.");

  // dont be silly, im still gonna send it
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);

  WiFi.mode(WIFI_STA);
  disconnectWiFi();

  M5.Lcd.clear(TFT_BLACK);
}

void loop()
{
  sweepForBleakest();
  for (int scannerIndex = 0; scannerIndex < seenScanners; scannerIndex++)
  {
    bool isHealthy = getHealthy(scannerIndex);
    if (!isHealthy)
    {
      Serial.print("Targetting scanner: ");
      Serial.println(scannerIndex);
      if (connectWiFi(scannerIndex))
      {
        if (getLogJson(scannerIndex))
        {
          updateScannerInList(healthStatusList[scannerIndex].mac);
        }
      }
      // disconnectWiFi();
    }
    updateLcd();
  }
  // delay(5000);
}
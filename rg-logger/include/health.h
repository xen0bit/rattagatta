#pragma once
#include <Arduino.h>
#include <WiFi.h>

#define MAX_HEALTH_ITEMS 15

struct HealthStatus
{
  uint8_t   mac[6];
  IPAddress ip;          // collector's IP on the AP subnet (set at registration)
  int       scannerIdx;  // assigned scanner index
  int       eventCount;
  unsigned long lastUpdated;
};

HealthStatus healthStatusList[MAX_HEALTH_ITEMS];
int seenScanners = 0;

// How long (ms) before a scanner is considered stale
static const unsigned long HEALTH_EXPIRY_MS = 10000;

bool isScannerInList(uint8_t mac[6])
{
  for (int i = 0; i < seenScanners; i++)
    if (memcmp(healthStatusList[i].mac, mac, 6) == 0) return true;
  return false;
}

// Returns assigned scannerIndex, or -1 if list is full.
int addScannerToList(uint8_t mac[6], IPAddress ip)
{
  if (seenScanners >= MAX_HEALTH_ITEMS)
  {
    Serial.println("Max scanners reached");
    return -1;
  }
  int idx = seenScanners;
  memcpy(healthStatusList[idx].mac, mac, 6);
  healthStatusList[idx].ip          = ip;
  healthStatusList[idx].scannerIdx  = idx;
  healthStatusList[idx].eventCount  = 0;
  healthStatusList[idx].lastUpdated = 0;
  seenScanners++;
  return idx;
}

int getScannerIndex(uint8_t mac[6])
{
  for (int i = 0; i < seenScanners; i++)
    if (memcmp(healthStatusList[i].mac, mac, 6) == 0) return i;
  return -1;
}

void updateScannerHealth(int idx)
{
  if (idx >= 0 && idx < seenScanners)
  {
    healthStatusList[idx].eventCount++;
    healthStatusList[idx].lastUpdated = millis();
  }
}

bool getHealthy(int idx)
{
  if (idx < 0 || idx >= seenScanners) return false;
  return (millis() - healthStatusList[idx].lastUpdated) < HEALTH_EXPIRY_MS;
}

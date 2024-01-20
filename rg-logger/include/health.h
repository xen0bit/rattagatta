#include <Arduino.h>

#define MAX_HEALTH_ITEMS 15

extern String ms;
extern uint8_t mb[6];

struct HealthStatus
{
  uint8_t mac[6];
  uint32_t channel;
  int eventCount;
  unsigned long lastUpdated;
};

HealthStatus healthStatusList[MAX_HEALTH_ITEMS];
int seenScanners = 0;
int expiration = 60000;

bool isScannerInList(uint8_t mac[6])
{
  for (int i = 0; i < MAX_HEALTH_ITEMS; i++)
  {
    if (memcmp(healthStatusList[i].mac, mac, 6) == 0)
    {
      return true;
    }
  }
  return false;
}

void addScannerToList(uint8_t mac[6], int32_t channel)
{
  memcpy(healthStatusList[seenScanners].mac, mac, 6);
  healthStatusList[seenScanners].channel = channel;
  healthStatusList[seenScanners].eventCount = 0;
  healthStatusList[seenScanners].lastUpdated = 0;
  seenScanners++;
}

int getScannerIndex(uint8_t mac[6])
{
  for (int i = 0; i < MAX_HEALTH_ITEMS; i++)
  {
    if (memcmp(healthStatusList[i].mac, mac, 6) == 0)
    {
      return i;
    }
  }
  // Default
  return 0;
}

void updateScannerInList(uint8_t mac[6])
{
  for (int i = 0; i < MAX_HEALTH_ITEMS; i++)
  {
    if (memcmp(healthStatusList[i].mac, mac, 6) == 0)
    {
      healthStatusList[i].eventCount++;
      healthStatusList[i].lastUpdated = millis();
    }
  }
}

bool getHealthy(int scannerIndex)
{
  unsigned long ct = millis();
  return healthStatusList[scannerIndex].lastUpdated + expiration > ct;
}

int getHealthyCount()
{
  // unsigned long ct = millis();
  int healthyCount = 0;
  for (int i = 0; i < seenScanners; i++)
  {
    if (getHealthy(i))
    {
      healthyCount++;
    }
  }
  return healthyCount;
}

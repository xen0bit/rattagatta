#include <Arduino.h>
#include <NimBLEDevice.h>
#include <CRC32.h>

// Rate Limiting
// Define the maximum number of allowed MAC addresses.
#define MAX_RATE_LIMIT_ITEMS 100
// Define the expiration time in seconds.
#define EXPIRATION_TIME 300

// Structure to store id and its expiration time.
struct RateLimit
{
  // id is CRC32 of either mac (public address) or manufacturer data (random address)
  uint32_t id;
  unsigned long expiration;
};

// Array to store the id and their expiration times.
RateLimit rateLimitList[MAX_RATE_LIMIT_ITEMS];

// Function to check if a id is already in the list.
bool isIdInList(uint32_t id)
{
  for (int i = 0; i < MAX_RATE_LIMIT_ITEMS; i++)
  {
    if (rateLimitList[i].id == id)
    {
      return true;
    }
  }
  return false;
}

// Function to add a new id to the list.
void addIdToList(uint32_t id)
{
  bool foundEmpty = false;
  int oldestEntry = ULONG_MAX;
  int writeIndex = 0;

  for (int i = 0; i < MAX_RATE_LIMIT_ITEMS; i++)
  {
    // Fill expired slots
    if (rateLimitList[i].expiration == 0)
    {
      rateLimitList[i].id = id;
      rateLimitList[i].expiration = millis() + (EXPIRATION_TIME * 1000);
      foundEmpty = true;
      break;
    }
    // Keep track of oldest entry as writeIndex
    else if (rateLimitList[i].expiration < oldestEntry)
    {
      oldestEntry = rateLimitList[i].expiration;
      writeIndex = i;
    }
  }

  // If empty slot wasn't found, replace the oldest
  if (!foundEmpty)
  {
    rateLimitList[writeIndex].id = id;
    rateLimitList[writeIndex].expiration = millis() + (EXPIRATION_TIME * 1000);
  }
}

// Function to remove expired MAC addresses from the list.
void removeExpiredIds()
{
  unsigned long currentMillis = millis();
  for (int i = 0; i < MAX_RATE_LIMIT_ITEMS; i++)
  {
    if (rateLimitList[i].expiration != 0 && rateLimitList[i].expiration < currentMillis)
    {
      Serial.print("Expired: ");
      Serial.println(rateLimitList[i].id);
      rateLimitList[i].id = 0;
      ;
      rateLimitList[i].expiration = 0;
    }
  }
}

// Function to check if a connection is allowed based on MAC address.
bool isConnectionAllowed(uint32_t id)
{
  removeExpiredIds();
  if (!isIdInList(id))
  {
    addIdToList(id);
    return true;
  }
  return false;
}

// Check if we are the owning scanner of the remote address
bool getOwnership(uint32_t id, int scannerIndex, int scannerCount)
{
  if (id % scannerCount == scannerIndex)
  {
    return true;
  }
  return false;
}

uint32_t getRateLimitId(NimBLEAdvertisedDevice *advertisedDevice)
{
  uint32_t checksum;
  // Public mac addr do not change, suitable for rate limit key
  if (advertisedDevice->getAddressType() == BLE_ADDR_PUBLIC)
  {
    checksum = CRC32::calculate(advertisedDevice->getAddress().getNative(), 6);
  }
  // Random mac addr do change, unsuitable for rate limit key
  // Pack manufacturer data into uint8_t[6] instead, which has a max length of 31 bytes
  // but we'll use 32 for easy maths
  else if (advertisedDevice->getAddressType() == BLE_ADDR_RANDOM)
  {
    checksum = CRC32::calculate(advertisedDevice->getManufacturerData().data(), advertisedDevice->getManufacturerData().length());
  }
  else
  {
    // Honestly, fuck every other advertisement type, default to mac bheavior
    checksum = CRC32::calculate(advertisedDevice->getAddress().getNative(), 6);
  }
  return checksum;
}
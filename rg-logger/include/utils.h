#include <Arduino.h>
#include <string.h>

void macStringToBytes(uint8_t mac[6], String macString)
{
  char *ptr;
  // Don't need strtok as long as you split out first
  mac[0] = strtol(macString.c_str(), &ptr, HEX);
  for (uint8_t i = 1; i < 6; i++)
  {
    mac[i] = strtol(ptr + 1, &ptr, HEX);
  }
}
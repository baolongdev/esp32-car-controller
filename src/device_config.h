#pragma once

#include <Arduino.h>

struct DeviceConfig {
  const char* bleName;
  const char* deviceId;
  const char* blePassword;
  uint8_t leftIn1;
  uint8_t leftIn2;
  uint8_t rightIn1;
  uint8_t rightIn2;
  bool leftMotorInverted;
  bool rightMotorInverted;
  int8_t debugLedPin;
  uint8_t servoPin;
};

constexpr uint8_t DEVICE_COUNT = 6;

// Chon thiet bi dang nap code bang cach doi chi so nay tu 0 den 5.
constexpr uint8_t ACTIVE_DEVICE_INDEX = 1;

extern const DeviceConfig DEVICE_CONFIGS[DEVICE_COUNT];
extern const DeviceConfig& ACTIVE_DEVICE;

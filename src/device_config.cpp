#include "device_config.h"

// Cau hinh 6 xe. Neu dau day khac nhau, chi can sua tai day.
const DeviceConfig DEVICE_CONFIGS[DEVICE_COUNT] = {
  {"ESP32-MOTOR-01", "CAR-01", "123456", 3, 4, 5, 6, false, true, 48, 7},
  {"ESP32-MOTOR-02", "CAR-02", "123456", 3, 4, 5, 6, false, true, 48, 7},
  {"ESP32-MOTOR-03", "CAR-03", "123456", 3, 4, 5, 6, false, true, 48, 7},
  {"ESP32-MOTOR-04", "CAR-04", "123456", 3, 4, 5, 6, false, true, 48, 7},
  {"ESP32-MOTOR-05", "CAR-05", "123456", 3, 4, 5, 6, false, true, 48, 7},
  {"ESP32-MOTOR-06", "CAR-06", "123456", 3, 4, 5, 6, false, true, 48, 7}
};

static_assert(ACTIVE_DEVICE_INDEX < DEVICE_COUNT, "ACTIVE_DEVICE_INDEX khong hop le");
const DeviceConfig& ACTIVE_DEVICE = DEVICE_CONFIGS[ACTIVE_DEVICE_INDEX];

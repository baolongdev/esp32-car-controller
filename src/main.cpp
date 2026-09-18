#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "device_config.h"

constexpr uint32_t PWM_FREQUENCY = 20000;
constexpr uint8_t PWM_RESOLUTION = 8;
constexpr bool STARTUP_MOTOR_TEST = false;
constexpr uint8_t DEBUG_LED_COUNT = 1;
// Log chi tiết từng packet có thể làm nghẽn USB CDC khi monitor đóng hoặc
// khi web gửi burst. Bật lại thành true khi cần debug bằng Serial Monitor.
constexpr bool DETAILED_SERIAL_LOG = false;
constexpr char FIRMWARE_VERSION[] = "motor-servo-ledc-v4";
constexpr uint32_t SERVO_PWM_FREQUENCY = 50;
// ESP32-S3 LEDC tối đa 14-bit; 16-bit khiến ledcAttachChannel() FAIL.
constexpr uint8_t SERVO_PWM_RESOLUTION = 14;
// Phân bổ cố định, không dùng LEDC_AUTO_CHANNEL:
// channel 0..3 cho 4 ngõ motor, channel 4 cho servo.
constexpr uint8_t MOTOR_PWM_CHANNEL_L1 = 0;
constexpr uint8_t MOTOR_PWM_CHANNEL_L2 = 1;
constexpr uint8_t MOTOR_PWM_CHANNEL_R1 = 2;
constexpr uint8_t MOTOR_PWM_CHANNEL_R2 = 3;
constexpr uint8_t SERVO_PWM_CHANNEL = 4;
constexpr uint16_t SERVO_MIN_PULSE_US = 500;
constexpr uint16_t SERVO_MAX_PULSE_US = 2400;

// Gửi tối đa ~40 state/s. Các state cũ chưa gửi sẽ bị state mới ghi đè,
// nhờ đó BLE TX không bị backlog khi web kéo slider/gửi lệnh liên tục.
constexpr uint32_t BLE_NOTIFY_MIN_INTERVAL_MS = 25;
constexpr bool VERBOSE_BLE_RX = false;

Adafruit_NeoPixel debugLed(
    DEBUG_LED_COUNT,
    ACTIVE_DEVICE.debugLedPin,
    NEO_GRB + NEO_KHZ800);
int servoAngle = 0;
bool servoReady = false;
bool motorsReady = false;
volatile int pendingServoAngle = -1;
portMUX_TYPE servoCommandMux = portMUX_INITIALIZER_UNLOCKED;

constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

BLECharacteristic* bleTx = nullptr;
BLEServer* bleServer = nullptr;

bool bleConnected = false;
bool bleAuthorized = false;
uint16_t authorizedConnId = ESP_GATT_IF_NONE;

// BLE callbacks must stay short. Incoming commands are copied to this queue
// and processed by a dedicated FreeRTOS task instead of the BLE host task.
struct CommandPacket {
  char data[40];
  uint16_t connId;
  bool fromSerial;
};

QueueHandle_t commandQueue = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
volatile bool ledStopRequested = false;
volatile bool advertisingRestartRequested = false;

// Trùng với giá trị mặc định của thanh tốc độ trong giao diện web.
int motorSpeed = 0;
String commandLine;

// Trạng thái chuyển động hiện tại: f / b / l / r / s.
char currentMotion = 's';

// LED không còn timer 1.5 s. LED giữ màu theo hướng hiện tại và tắt khi stop.
bool debugLedOn = false;
uint32_t debugLedColor = 0;

// BLE TX coalescing: chỉ giữ message mới nhất thay vì notify mọi message trung gian.
char pendingBleMessage[48] = {0};
bool bleMessagePending = false;
uint32_t lastBleNotifyAt = 0;

void clearDebugLed();
void setStoppedLed();
void processCommandPacket(const CommandPacket& packet);

bool enqueueCommand(const char* data, uint16_t connId, bool fromSerial) {
  if (commandQueue == nullptr || data == nullptr) return false;

  CommandPacket packet{};
  strncpy(packet.data, data, sizeof(packet.data) - 1);
  packet.data[sizeof(packet.data) - 1] = '\0';
  packet.connId = connId;
  packet.fromSerial = fromSerial;

  // Never block the BLE callback. If a burst fills the queue, discard the
  // oldest packet and keep the newest command for lower control latency.
  if (xQueueSend(commandQueue, &packet, 0) == pdTRUE) return true;

  CommandPacket discarded{};
  xQueueReceive(commandQueue, &discarded, 0);
  return xQueueSend(commandQueue, &packet, 0) == pdTRUE;
}

// -----------------------------------------------------------------------------
// LED
// -----------------------------------------------------------------------------

void setDebugLedColor(uint32_t color) {
  if (ACTIVE_DEVICE.debugLedPin < 0) return;

  // Không gọi NeoPixel.show() lại nếu màu không đổi.
  if (debugLedOn && debugLedColor == color) return;

  debugLedColor = color;
  debugLedOn = true;
  debugLed.setPixelColor(0, color);
  debugLed.show();
}

void clearDebugLed() {
  if (ACTIVE_DEVICE.debugLedPin < 0) return;
  if (!debugLedOn) return;

  debugLedOn = false;
  debugLedColor = 0;
  debugLed.setPixelColor(0, 0);
  debugLed.show();
}

void setStoppedLed() {
  // Xe dừng / mất BLE => LED tắt ngay.
  clearDebugLed();
}

void updateMotionLed(char command, int pwm) {
  if (pwm <= 0 || command == 's') {
    setStoppedLed();
    return;
  }

  switch (command) {
    case 'f': setDebugLedColor(debugLed.Color(0, 80, 255)); break;     // Forward: blue
    case 'b': setDebugLedColor(debugLed.Color(160, 0, 255)); break;    // Backward: purple
    case 'l': setDebugLedColor(debugLed.Color(255, 150, 0)); break;    // Left: orange
    case 'r': setDebugLedColor(debugLed.Color(0, 220, 180)); break;    // Right: cyan
    default: break;
  }
}

// -----------------------------------------------------------------------------
// Motor
// -----------------------------------------------------------------------------

void stopMotors();

void forceMotorPinsLow() {
  pinMode(ACTIVE_DEVICE.leftIn1, OUTPUT);
  pinMode(ACTIVE_DEVICE.leftIn2, OUTPUT);
  pinMode(ACTIVE_DEVICE.rightIn1, OUTPUT);
  pinMode(ACTIVE_DEVICE.rightIn2, OUTPUT);

  digitalWrite(ACTIVE_DEVICE.leftIn1, LOW);
  digitalWrite(ACTIVE_DEVICE.leftIn2, LOW);
  digitalWrite(ACTIVE_DEVICE.rightIn1, LOW);
  digitalWrite(ACTIVE_DEVICE.rightIn2, LOW);
}

bool initMotors() {
  forceMotorPinsLow();

  const uint8_t pins[] = {
      ACTIVE_DEVICE.leftIn1,
      ACTIVE_DEVICE.leftIn2,
      ACTIVE_DEVICE.rightIn1,
      ACTIVE_DEVICE.rightIn2};
  const uint8_t channels[] = {
      MOTOR_PWM_CHANNEL_L1,
      MOTOR_PWM_CHANNEL_L2,
      MOTOR_PWM_CHANNEL_R1,
      MOTOR_PWM_CHANNEL_R2};

  bool attached = true;
  for (size_t i = 0; i < 4; ++i) {
    const bool pinAttached = ledcAttachChannel(
        pins[i], PWM_FREQUENCY, PWM_RESOLUTION, channels[i]);
    attached = attached && pinAttached;
    Serial.printf("Motor PWM GPIO%d channel=%d: %s\n",
                  pins[i], channels[i],
                  pinAttached ? "OK" : "FAIL");
  }

  motorsReady = attached;
  if (motorsReady) {
    stopMotors();
  } else {
    // Nếu LEDC attach lỗi, vẫn giữ driver ở trạng thái an toàn.
    forceMotorPinsLow();
  }

  Serial.printf("Motor init: %s\n", motorsReady ? "OK" : "FAIL");
  return motorsReady;
}

int applyMotorInvert(int speed, bool inverted) {
  return inverted ? -speed : speed;
}

void writeMotor(uint8_t in1, uint8_t in2, int speed) {
  speed = constrain(speed, -255, 255);

  if (!motorsReady) {
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    return;
  }

  if (speed > 0) {
    ledcWrite(in1, speed);
    ledcWrite(in2, 0);
  } else if (speed < 0) {
    ledcWrite(in1, 0);
    ledcWrite(in2, -speed);
  } else {
    ledcWrite(in1, 0);
    ledcWrite(in2, 0);
  }
}

void setMotors(int left, int right) {
  writeMotor(
      ACTIVE_DEVICE.leftIn1,
      ACTIVE_DEVICE.leftIn2,
      applyMotorInvert(
          constrain(left, -255, 255),
          ACTIVE_DEVICE.leftMotorInverted));

  writeMotor(
      ACTIVE_DEVICE.rightIn1,
      ACTIVE_DEVICE.rightIn2,
      applyMotorInvert(
          constrain(right, -255, 255),
          ACTIVE_DEVICE.rightMotorInverted));
}

void stopMotors() { setMotors(0, 0); }
void moveForward(int pwm) { setMotors(pwm, pwm); }
void moveBackward(int pwm) { setMotors(-pwm, -pwm); }
void turnLeft(int pwm) { setMotors(-pwm, pwm); }
void turnRight(int pwm) { setMotors(pwm, -pwm); }

void setServoAngle(int angle);

void initServo() {
  if (servoReady) return;

  servoReady = ledcAttachChannel(
      ACTIVE_DEVICE.servoPin,
      SERVO_PWM_FREQUENCY,
      SERVO_PWM_RESOLUTION,
      SERVO_PWM_CHANNEL);
  Serial.printf("Servo init GPIO%d channel=%d: %s\n",
                ACTIVE_DEVICE.servoPin, SERVO_PWM_CHANNEL,
                servoReady ? "OK" : "FAIL");

  if (servoReady) {
    setServoAngle(0);
  }
}

void setServoAngle(int angle) {
  if (!servoReady) initServo();
  if (!servoReady) return;

  servoAngle = constrain(angle, 0, 180);
  const uint32_t pulseUs = map(
      servoAngle, 0, 180, SERVO_MIN_PULSE_US, SERVO_MAX_PULSE_US);
  const uint32_t duty = (pulseUs * ((1UL << SERVO_PWM_RESOLUTION) - 1)) /
                        (1000000UL / SERVO_PWM_FREQUENCY);
  ledcWrite(ACTIVE_DEVICE.servoPin, duty);
  Serial.printf("[SERVO %lu] angle=%d pulse=%luus duty=%lu\n",
                millis(), servoAngle, pulseUs, duty);
}

void queueServoAngle(int angle) {
  angle = constrain(angle, 0, 180);
  portENTER_CRITICAL(&servoCommandMux);
  pendingServoAngle = angle;
  portEXIT_CRITICAL(&servoCommandMux);
  if (DETAILED_SERIAL_LOG) {
    Serial.printf("[SERVO QUEUE %lu] angle=%d\n", millis(), angle);
  }
}

void processPendingServo() {
  int angle = -1;
  portENTER_CRITICAL(&servoCommandMux);
  if (pendingServoAngle >= 0) {
    angle = pendingServoAngle;
    pendingServoAngle = -1;
  }
  portEXIT_CRITICAL(&servoCommandMux);

  if (angle >= 0) {
    if (DETAILED_SERIAL_LOG) {
      Serial.printf("[SERVO PROCESS %lu] angle=%d\n", millis(), angle);
    }
    setServoAngle(angle);
  }
}

void logMotion(char command, int pwm) {
  if (!DETAILED_SERIAL_LOG) return;

  int left = 0;
  int right = 0;
  switch (command) {
    case 'f': left = pwm; right = pwm; break;
    case 'b': left = -pwm; right = -pwm; break;
    case 'l': left = -pwm; right = pwm; break;
    case 'r': left = pwm; right = -pwm; break;
    default: break;
  }

  const int appliedLeft = applyMotorInvert(left, ACTIVE_DEVICE.leftMotorInverted);
  const int appliedRight = applyMotorInvert(right, ACTIVE_DEVICE.rightMotorInverted);
  Serial.printf(
      "[MOTOR %lu] cmd=%c pwm=%d left=%d right=%d ready=%s\n",
      millis(), command, pwm, appliedLeft, appliedRight,
      motorsReady ? "YES" : "NO");
}

void applyMotion(char command, int pwm) {
  pwm = constrain(pwm, 0, 255);

  // PWM=0 luôn được coi là STOP để trạng thái motor và LED không lệch nhau.
  if (pwm == 0 || command == 's') {
    stopMotors();
    currentMotion = 's';
    updateMotionLed('s', 0);
    logMotion('s', 0);
    return;
  }

  switch (command) {
    case 'f': moveForward(pwm); break;
    case 'b': moveBackward(pwm); break;
    case 'l': turnLeft(pwm); break;
    case 'r': turnRight(pwm); break;
    default: return;
  }

  currentMotion = command;
  updateMotionLed(command, pwm);
  logMotion(command, pwm);
}

// -----------------------------------------------------------------------------
// BLE TX
// -----------------------------------------------------------------------------

void sendBleAuthMessage(const char* message) {
  // AUTH cần phản hồi ngay vì web thường đang chờ kết quả xác thực.
  if (bleTx == nullptr) return;
  bleTx->setValue(message);
  bleTx->notify();
  if (DETAILED_SERIAL_LOG) {
    Serial.printf("[BLE TX %lu] auth=\"%s\"\n", millis(), message);
  }
}

void queueBleMessage(const char* message) {
  if (message == nullptr) return;

  strncpy(pendingBleMessage, message, sizeof(pendingBleMessage) - 1);
  pendingBleMessage[sizeof(pendingBleMessage) - 1] = '\0';
  bleMessagePending = true;
}

void flushBleMessage() {
  if (!bleMessagePending || !bleConnected || !bleAuthorized || bleTx == nullptr) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastBleNotifyAt < BLE_NOTIFY_MIN_INTERVAL_MS) return;

  bleTx->setValue(pendingBleMessage);
  bleTx->notify();

  if (DETAILED_SERIAL_LOG) {
    Serial.printf("[BLE TX %lu] notify=\"%s\"\n",
                  now, pendingBleMessage);
  }

  lastBleNotifyAt = now;
  bleMessagePending = false;
}

void queueMotionState(char command, int pwm) {
  char response[40];

  if (command == 's' || pwm <= 0) {
    snprintf(response, sizeof(response), "Dung PWM=0");
  } else {
    const char* name =
        command == 'f' ? "Tien" :
        command == 'b' ? "Lui" :
        command == 'l' ? "Quay trai" :
        command == 'r' ? "Quay phai" : "Unknown";

    snprintf(response, sizeof(response), "%s PWM=%d", name, pwm);
  }

  queueBleMessage(response);
}

// -----------------------------------------------------------------------------
// Serial helpers
// -----------------------------------------------------------------------------

void runTestFor(uint32_t durationMs) {
  Serial.printf(
      "Test tien 2 motor trong %lu giay, PWM=%d\n",
      durationMs / 1000,
      motorSpeed);

  applyMotion('f', motorSpeed);
  const uint32_t startedAt = millis();

  while (millis() - startedAt < durationMs) {
    flushBleMessage();
    delay(1);
  }

  applyMotion('s', 0);
  Serial.println(F("Ket thuc test - 2 motor dung"));
}

void printHelp() {
  Serial.println(F("f= tien, b= lui, l= quay trai, r= quay phai, s= dung"));
  Serial.println(F("Lenh co PWM: f100, b100, l150, r150"));
  Serial.println(F("t= test tien 10 giay, += tang, -= giam, x= dung"));
}

void handleCommand(char command) {
  if (command == '\r' || command == '\n' || command == ' ') return;

  command = static_cast<char>(
      tolower(static_cast<unsigned char>(command)));

  switch (command) {
    case 'f':
      applyMotion('f', motorSpeed);
      Serial.printf("Tien, PWM=%d\n", motorSpeed);
      break;

    case 'b':
      applyMotion('b', motorSpeed);
      Serial.printf("Lui, PWM=%d\n", motorSpeed);
      break;

    case 'l':
      applyMotion('l', motorSpeed);
      Serial.printf("Quay trai, PWM=%d\n", motorSpeed);
      break;

    case 'r':
      applyMotion('r', motorSpeed);
      Serial.printf("Quay phai, PWM=%d\n", motorSpeed);
      break;

    case 's':
      applyMotion('s', 0);
      Serial.println(F("Dung 2 motor"));
      break;

    case 't':
      runTestFor(10000);
      break;

    case 'x':
      applyMotion('s', 0);
      Serial.println(F("Dung khan cap"));
      break;

    case '+':
      motorSpeed = min(255, motorSpeed + 15);
      Serial.printf("PWM=%d\n", motorSpeed);
      break;

    case '-':
      motorSpeed = max(0, motorSpeed - 15);
      Serial.printf("PWM=%d\n", motorSpeed);
      break;

    default:
      if (command >= '0' && command <= '9') {
        motorSpeed = map(command - '0', 0, 9, 0, 255);
        Serial.printf("PWM=%d\n", motorSpeed);
      }
      break;
  }
}

// -----------------------------------------------------------------------------
// BLE RX
// -----------------------------------------------------------------------------

void handleLine(
    const char* rawLine,
    uint16_t connId = ESP_GATT_IF_NONE,
    bool fromSerial = false) {
  if (rawLine == nullptr) return;

  // Parse bằng buffer cố định để BLE command không tạo nhiều String tạm,
  // tránh phân mảnh heap sau thời gian chạy dài.
  char line[sizeof(CommandPacket::data)] = {};
  strncpy(line, rawLine, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  char* start = line;
  while (*start != '\0' && isspace(static_cast<unsigned char>(*start))) {
    ++start;
  }
  char* end = start + strlen(start);
  while (end > start && isspace(static_cast<unsigned char>(end[-1]))) {
    --end;
    *end = '\0';
  }
  if (*start == '\0') return;

  if (VERBOSE_BLE_RX || DETAILED_SERIAL_LOG) {
    Serial.printf("[RX %lu] source=%s conn=%u auth=%s data=\"%s\"\n",
                  millis(), fromSerial ? "SERIAL" : "BLE", connId,
                  bleAuthorized ? "YES" : "NO", start);
  }

  // AUTH xử lý trong controlTask; callback BLE chỉ enqueue dữ liệu.
  if (strncmp(start, "AUTH:", 5) == 0) {
    if (strcmp(start + 5, ACTIVE_DEVICE.blePassword) == 0) {
      if (connId != ESP_GATT_IF_NONE &&
          bleAuthorized &&
          authorizedConnId != ESP_GATT_IF_NONE &&
          authorizedConnId != connId &&
          bleServer != nullptr) {
        bleServer->disconnect(authorizedConnId);
      }

      authorizedConnId = connId;
      bleAuthorized = true;
      bleConnected = true;

      sendBleAuthMessage("AUTH OK");
      Serial.printf("BLE auth OK, connId=%u\n", connId);
    } else {
      sendBleAuthMessage("AUTH FAIL");
      Serial.printf("BLE auth FAIL, connId=%u\n", connId);

      if (bleServer != nullptr && connId != ESP_GATT_IF_NONE) {
        bleServer->disconnect(connId);
      }
    }
    return;
  }

  if (!fromSerial &&
      (!bleAuthorized ||
      (connId != ESP_GATT_IF_NONE &&
       authorizedConnId != ESP_GATT_IF_NONE &&
       connId != authorizedConnId))) {
    sendBleAuthMessage("AUTH REQUIRED");
    return;
  }

  const char command = static_cast<char>(
      tolower(static_cast<unsigned char>(start[0])));

  if (command == 'v' && start[1] != '\0') {
    const int angle = constrain(atoi(start + 1), 0, 180);
    queueServoAngle(angle);
    char response[32];
    snprintf(response, sizeof(response), "SERVO=%d", angle);
    queueBleMessage(response);
    return;
  }

  // f100 / b100 / l150 / r150
  if ((command == 'f' || command == 'b' || command == 'l' || command == 'r') &&
      start[1] != '\0') {
    motorSpeed = constrain(atoi(start + 1), 0, 255);

    // Motor đổi NGAY khi RX đến. Không chờ notify và không chờ loop timer.
    applyMotion(command, motorSpeed);
    if (DETAILED_SERIAL_LOG) {
      Serial.printf("[COMMAND %lu] motor=%c requested_pwm=%d\n",
                    millis(), command, motorSpeed);
    }

    // TX chỉ giữ state mới nhất, tránh backlog khi slider gửi dày.
    queueMotionState(currentMotion, motorSpeed);
    return;
  }

  if (command == 's') {
    applyMotion('s', 0);
    queueMotionState('s', 0);
    if (DETAILED_SERIAL_LOG) {
      Serial.printf("[COMMAND %lu] stop requested\n", millis());
    }
    return;
  }

  // Giữ tương thích với các lệnh đơn cũ.
  handleCommand(command);
}

// -----------------------------------------------------------------------------
// BLE callbacks
// -----------------------------------------------------------------------------

void onBleConnected(uint16_t connId = ESP_GATT_IF_NONE) {
  bleConnected = true;

  // Kết nối nhưng chưa chạy => dừng motor ngay. LED/Serial xử lý ngoài
  // callback để không chặn BLE host task.
  stopMotors();
  ledStopRequested = true;
  Serial.printf("BLE da ket noi, connId=%u - dang cho mat khau\n", connId);
}

void onBleDisconnected(BLEServer* server, uint16_t connId = ESP_GATT_IF_NONE) {
  // Fail-safe: dừng PWM ngay. Các thao tác LED/advertising được defer.
  // Không gọi NeoPixel.show() hoặc BLE API dài trong callback.
  stopMotors();
  currentMotion = 's';
  ledStopRequested = true;

  if (connId == ESP_GATT_IF_NONE ||
      authorizedConnId == ESP_GATT_IF_NONE ||
      connId == authorizedConnId) {
    authorizedConnId = ESP_GATT_IF_NONE;
    bleAuthorized = false;
  }

  bleMessagePending = false;
  bleConnected = server != nullptr && server->getConnectedCount() > 0;

  Serial.printf("BLE ngat ket noi, connId=%u\n", connId);
  advertisingRestartRequested = true;
}

class BleServerCallbacks : public BLEServerCallbacks {
 public:
  void onConnect(BLEServer*) override {
    onBleConnected();
  }

  void onDisconnect(BLEServer* server) override {
    onBleDisconnected(server);
  }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    onBleConnected(param->connect.conn_id);
  }

  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    onBleDisconnected(server, param->disconnect.conn_id);
  }
#endif
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
 public:
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      enqueueCommand(value.c_str(), ESP_GATT_IF_NONE, false);
    }
  }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(
      BLECharacteristic* characteristic,
      esp_ble_gatts_cb_param_t* param) override {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      enqueueCommand(value.c_str(), param->write.conn_id, false);
    }
  }
#endif
};

void setupBle() {
  BLEDevice::init(ACTIVE_DEVICE.bleName);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());

  BLEService* service = bleServer->createService(BLE_SERVICE_UUID);

  BLECharacteristic* rx = service->createCharacteristic(
      BLE_RX_UUID,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_WRITE_NR);

  bleTx = service->createCharacteristic(
      BLE_TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);

  rx->setCallbacks(new BleRxCallbacks());

  service->start();
  BLEAdvertising* advertising = bleServer->getAdvertising();
  advertising->setName(ACTIVE_DEVICE.bleName);
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  // Đưa tên thiết bị vào scan response để Chrome/Web Bluetooth lọc được
  // namePrefix ESP32-MOTOR ngay cả khi gói quảng bá chính đã đầy.
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  advertising->start();

  setStoppedLed();

  Serial.printf(
      "BLE san sang: %s (%s)\n",
      ACTIVE_DEVICE.bleName,
      ACTIVE_DEVICE.deviceId);
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------

void processCommandPacket(const CommandPacket& packet) {
  handleLine(packet.data, packet.connId, packet.fromSerial);
}

void controlTask(void*) {
  CommandPacket packet{};

  for (;;) {
    if (ledStopRequested) {
      ledStopRequested = false;
      setStoppedLed();
    }

    if (advertisingRestartRequested && bleServer != nullptr) {
      advertisingRestartRequested = false;
      bleServer->getAdvertising()->start();
    }

    // Drain a small burst without blocking. The queue is bounded and old
    // packets are discarded when full, so slider/button spam cannot create a
    // long FIFO delay.
    uint8_t processed = 0;
    while (processed < 4 &&
           xQueueReceive(commandQueue, &packet, 0) == pdTRUE) {
      processCommandPacket(packet);
      ++processed;
    }

    processPendingServo();
    flushBleMessage();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup() {
  Serial.begin(115200);

  commandQueue = xQueueCreate(12, sizeof(CommandPacket));
  if (commandQueue == nullptr) {
    Serial.println(F("ERROR: command queue allocation failed"));
  } else {
    // Core 1 is also where Arduino loopTask normally runs. A dedicated task
    // keeps BLE callbacks short while preserving deterministic motor control.
    const BaseType_t taskCreated = xTaskCreatePinnedToCore(
        controlTask,
        "controlTask",
        6144,
        nullptr,
        3,
        &controlTaskHandle,
        1);
    Serial.printf("FreeRTOS control task: %s\n",
                  taskCreated == pdPASS ? "OK" : "FAIL");
  }

  // Đưa các ngõ vào DRV8833 về LOW ngay khi MCU bắt đầu chạy.
  // Không chờ BLE/servo để tránh bánh xe giật hoặc quay lúc khởi động.
  forceMotorPinsLow();
  delay(300);
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

  if (ACTIVE_DEVICE.debugLedPin >= 0) {
    debugLed.begin();
    debugLed.setBrightness(45);
    debugLed.clear();
    debugLed.show();
  }

  // Motor luôn khởi động ở PWM=0 trước khi có BLE command.
  initMotors();

  // Advertising BLE trước servo: nếu servo có lỗi, BLE vẫn phải scan/connect.
  Serial.println(F(">>> Starting BLE"));
  setupBle();
  Serial.println(F(">>> BLE started"));

  // Servo dùng LEDC trực tiếp trên channel riêng, không gọi ESP32Servo.attach().
  // Việc này không chặn BLE và đặt góc ban đầu về 0 độ.
  initServo();

  Serial.println(F("ESP32-S3 + DRV8833 differential drive ready"));
  printHelp();

  if (STARTUP_MOTOR_TEST) {
    runTestFor(10000);
  }
}

void loop() {
  while (Serial.available() > 0) {
    const char received = static_cast<char>(Serial.read());

    if (received == '\r' || received == '\n') {
      // USB Serial dùng để debug/test trực tiếp, không cần BLE AUTH.
      enqueueCommand(commandLine.c_str(), ESP_GATT_IF_NONE, true);
      commandLine = "";
    } else if (commandLine.length() < 15) {
      commandLine += received;
    }
  }
}

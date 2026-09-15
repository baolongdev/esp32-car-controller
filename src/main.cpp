#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Servo.h>
#include "device_config.h"

constexpr uint32_t PWM_FREQUENCY = 20000;
constexpr uint8_t PWM_RESOLUTION = 8;
constexpr bool STARTUP_MOTOR_TEST = false;
constexpr uint8_t DEBUG_LED_COUNT = 1;

// Gửi tối đa ~40 state/s. Các state cũ chưa gửi sẽ bị state mới ghi đè,
// nhờ đó BLE TX không bị backlog khi web kéo slider/gửi lệnh liên tục.
constexpr uint32_t BLE_NOTIFY_MIN_INTERVAL_MS = 25;
constexpr bool VERBOSE_BLE_RX = false;

Adafruit_NeoPixel debugLed(
    DEBUG_LED_COUNT,
    ACTIVE_DEVICE.debugLedPin,
    NEO_GRB + NEO_KHZ800);
Servo steeringServo;
int servoAngle = 0;

constexpr char BLE_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char BLE_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

BLECharacteristic* bleTx = nullptr;
BLEServer* bleServer = nullptr;

bool bleConnected = false;
bool bleAuthorized = false;
uint16_t authorizedConnId = ESP_GATT_IF_NONE;

// Trùng với giá trị mặc định của thanh tốc độ trong giao diện web.
int motorSpeed = 180;
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

int applyMotorInvert(int speed, bool inverted) {
  return inverted ? -speed : speed;
}

void writeMotor(uint8_t in1, uint8_t in2, int speed) {
  speed = constrain(speed, -255, 255);

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

void setServoAngle(int angle) {
  servoAngle = constrain(angle, 0, 180);
  steeringServo.write(servoAngle);
  Serial.printf("Servo, goc=%d\n", servoAngle);
}

void applyMotion(char command, int pwm) {
  pwm = constrain(pwm, 0, 255);

  // PWM=0 luôn được coi là STOP để trạng thái motor và LED không lệch nhau.
  if (pwm == 0 || command == 's') {
    stopMotors();
    currentMotion = 's';
    updateMotionLed('s', 0);
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
}

// -----------------------------------------------------------------------------
// BLE TX
// -----------------------------------------------------------------------------

void sendBleAuthMessage(const char* message) {
  // AUTH cần phản hồi ngay vì web thường đang chờ kết quả xác thực.
  if (bleTx == nullptr) return;
  bleTx->setValue(message);
  bleTx->notify();
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

void handleLine(String line, uint16_t connId = ESP_GATT_IF_NONE) {
  line.trim();
  if (line.length() == 0) return;

  if (VERBOSE_BLE_RX) {
    Serial.printf("[RX] %s\n", line.c_str());
  }

  // AUTH vẫn xử lý ngay trong callback để web không timeout.
  if (line.startsWith("AUTH:")) {
    const String password = line.substring(5);

    if (password == ACTIVE_DEVICE.blePassword) {
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

  if (!bleAuthorized ||
      (connId != ESP_GATT_IF_NONE &&
       authorizedConnId != ESP_GATT_IF_NONE &&
       connId != authorizedConnId)) {
    sendBleAuthMessage("AUTH REQUIRED");
    return;
  }

  const char command = static_cast<char>(
      tolower(static_cast<unsigned char>(line[0])));

  if (command == 'v' && line.length() > 1) {
    setServoAngle(line.substring(1).toInt());
    char response[32];
    snprintf(response, sizeof(response), "SERVO=%d", servoAngle);
    queueBleMessage(response);
    return;
  }

  // f100 / b100 / l150 / r150
  if ((command == 'f' || command == 'b' || command == 'l' || command == 'r') &&
      line.length() > 1) {
    motorSpeed = constrain(line.substring(1).toInt(), 0, 255);

    // Motor đổi NGAY khi RX đến. Không chờ notify và không chờ loop timer.
    applyMotion(command, motorSpeed);

    // TX chỉ giữ state mới nhất, tránh backlog khi slider gửi dày.
    queueMotionState(currentMotion, motorSpeed);
    return;
  }

  if (command == 's') {
    applyMotion('s', 0);
    queueMotionState('s', 0);
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

  // Kết nối nhưng chưa chạy => LED vẫn tắt.
  setStoppedLed();

  Serial.printf("BLE da ket noi, connId=%u - dang cho mat khau\n", connId);
}

void onBleDisconnected(BLEServer* server, uint16_t connId = ESP_GATT_IF_NONE) {
  // Fail-safe: mất BLE thì xe dừng ngay và LED tắt ngay.
  applyMotion('s', 0);

  if (connId == ESP_GATT_IF_NONE ||
      authorizedConnId == ESP_GATT_IF_NONE ||
      connId == authorizedConnId) {
    authorizedConnId = ESP_GATT_IF_NONE;
    bleAuthorized = false;
  }

  bleMessagePending = false;
  bleConnected = server != nullptr && server->getConnectedCount() > 0;

  Serial.printf("BLE ngat ket noi, connId=%u\n", connId);

  if (server != nullptr) {
    server->getAdvertising()->start();
  }
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
      handleLine(value);
    }
  }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(
      BLECharacteristic* characteristic,
      esp_ble_gatts_cb_param_t* param) override {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      handleLine(value, param->write.conn_id);
    }
  }
#endif
};

void setupBle() {
  BLEDevice::init(ACTIVE_DEVICE.bleName);

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
  bleServer->getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  bleServer->getAdvertising()->start();

  setStoppedLed();

  Serial.printf(
      "BLE san sang: %s (%s)\n",
      ACTIVE_DEVICE.bleName,
      ACTIVE_DEVICE.deviceId);
}

// -----------------------------------------------------------------------------
// Setup / loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);

  if (ACTIVE_DEVICE.debugLedPin >= 0) {
    debugLed.begin();
    debugLed.setBrightness(45);
    debugLed.clear();
    debugLed.show();
  }

  for (uint8_t pin : {
           ACTIVE_DEVICE.leftIn1,
           ACTIVE_DEVICE.leftIn2,
           ACTIVE_DEVICE.rightIn1,
           ACTIVE_DEVICE.rightIn2}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    ledcAttach(pin, PWM_FREQUENCY, PWM_RESOLUTION);
  }

  steeringServo.setPeriodHertz(50);
  steeringServo.attach(ACTIVE_DEVICE.servoPin, 500, 2400);
  setServoAngle(0);

  stopMotors();

  Serial.println(F("ESP32-S3 + DRV8833 differential drive ready"));
  printHelp();

  setupBle();

  if (STARTUP_MOTOR_TEST) {
    runTestFor(10000);
  }
}

void loop() {
  // BLE TX được flush ngoài callback để callback RX luôn ngắn và motor phản hồi nhanh.
  flushBleMessage();

  while (Serial.available() > 0) {
    const char received = static_cast<char>(Serial.read());

    if (received == '\r' || received == '\n') {
      handleLine(commandLine);
      commandLine = "";
    } else if (commandLine.length() < 15) {
      commandLine += received;
    }
  }
}

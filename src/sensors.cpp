#include "sensors.h"
#include "config.h"

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

static Adafruit_MPU6050 mpu;

void sensorsInit() {
  // MPU6050 is on the I2C bus: SDA=GPIO21, SCL=GPIO22
  Wire.begin(21, 22);

  if (!mpu.begin()) {
    Serial.println("ERROR: MPU6050 not found. Check wiring (SDA=21, SCL=22, VCC=3.3V, GND=GND).");
    while (1) {
      delay(1000); // halt here; nothing else can run without the sensor
    }
  }
  Serial.println("MPU6050 found and initialized.");

  // Ranges wide enough to actually capture a crash-level impact without clipping.
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

void readCrashMetrics(float &totalAccel, float &totalGyro) {
  sensors_event_t accelEvent, gyroEvent, tempEvent;
  mpu.getEvent(&accelEvent, &gyroEvent, &tempEvent);

  // Total acceleration magnitude (m/s^2). At rest this should read ~9.8 (gravity).
  float ax = accelEvent.acceleration.x;
  float ay = accelEvent.acceleration.y;
  float az = accelEvent.acceleration.z;
  totalAccel = sqrt(ax * ax + ay * ay + az * az);

  // Total rotation magnitude, converted from rad/s to deg/s.
  float gx = gyroEvent.gyro.x * 180.0 / PI;
  float gy = gyroEvent.gyro.y * 180.0 / PI;
  float gz = gyroEvent.gyro.z * 180.0 / PI;
  totalGyro = sqrt(gx * gx + gy * gy + gz * gz);
}

int readBatteryPercent() {
  int raw = analogRead(BATTERY_PIN);
  float pinVoltage = (raw / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_DIVIDER_RATIO;
  float percent = (batteryVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0;
  return (int) constrain(percent, 0.0f, 100.0f);
}

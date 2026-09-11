#include "sensors.h"
#include "config.h"

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

static Adafruit_MPU6050 mpu;

void sensorsInit() {
  // MPU6050 is on the I2C bus: SDA=GPIO21, SCL=GPIO22.
  // 50 kHz is half the standard 100 kHz speed — this dramatically reduces
  // I2C timeout errors (Wire Error 263) caused by long jumper wires or a noisy
  // supply rail during WiFi TX bursts. Slower clock = longer bit times = more
  // tolerance for signal degradation.
  Wire.begin(21, 22);
  Wire.setClock(20000); // 20 kHz — extra slow for long/noisy jumper wires

  // Try default address 0x68 first (AD0 pin low), then 0x69 (AD0 pin high).
  if (!mpu.begin(0x68) && !mpu.begin(0x69)) {
    Serial.println("ERROR: MPU6050 not found on 0x68 or 0x69.");
    Serial.println("Check: SDA->GPIO21, SCL->GPIO22, VCC->3.3V, GND->GND.");
    Serial.println("Most common fix: SDA and SCL wires are swapped.");

    // I2C bus scan — shows which addresses actually respond
    Serial.println("Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  Found device at 0x%02X\n", addr);
      }
    }
    Serial.println("Scan done. If nothing found, check wiring and power.");

    while (1) {
      delay(1000);
    }
  }
  Serial.println("MPU6050 found and initialized.");

  // Ranges wide enough to actually capture a crash-level impact without clipping.
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

// Last successfully validated reading — returned when the current read looks stale.
static float sLastAccel = 9.81f;  // 1g at rest
static float sLastGyro  = 0.0f;
static float sPrevAccel = -1.0f;  // for duplicate detection
static float sPrevGyro  = -1.0f;
static uint8_t sDupCount = 0;

void readMotion(MotionReading &out) {
  sensors_event_t accelEvent, gyroEvent, tempEvent;
  mpu.getEvent(&accelEvent, &gyroEvent, &tempEvent);

  // Total acceleration magnitude (m/s^2). At rest this should read ~9.8 (gravity).
  float ax = accelEvent.acceleration.x;
  float ay = accelEvent.acceleration.y;
  float az = accelEvent.acceleration.z;
  float accel = sqrt(ax * ax + ay * ay + az * az);

  // Total rotation magnitude, converted from rad/s to deg/s.
  float gx = gyroEvent.gyro.x * 180.0 / PI;
  float gy = gyroEvent.gyro.y * 180.0 / PI;
  float gz = gyroEvent.gyro.z * 180.0 / PI;
  float gyro = sqrt(gx * gx + gy * gy + gz * gz);

  out.ax = ax;  out.ay = ay;  out.az = az;
  out.gx = gx;  out.gy = gy;  out.gz = gz;
  out.tempC = tempEvent.temperature;
  out.substituted = false;

  // Stale-data guard: when I2C fails mid-transfer, mpu.getEvent() leaves the
  // event structs unchanged (the previous buffer contents). The giveaway is an
  // exact floating-point match with the prior sample. Two identical consecutive
  // readings above the precaution threshold are treated as stale and replaced
  // with the last known-good values instead of triggering a false alert.
  // (Two-reading window avoids false-flagging a genuine sustained impact.)
  if (accel == sPrevAccel && gyro == sPrevGyro) {
    sDupCount++;
  } else {
    sDupCount = 0;
  }
  sPrevAccel = accel;
  sPrevGyro  = gyro;

  // A reading below 2 m/s² is physically impossible for a stationary sensor —
  // gravity alone contributes ~9.81. This means the I2C read failed silently
  // and returned zeros. Treat it as invalid and keep the last good value.
  bool tooLow = (accel < 2.0f);

  bool looksStale = (sDupCount >= 2) &&
                    (accel > PRECAUTION_ACCEL_THRESHOLD || gyro > PRECAUTION_GYRO_THRESHOLD);

  if (tooLow || looksStale) {
    out.totalAccel  = sLastAccel;
    out.totalGyro   = sLastGyro;
    out.substituted = true;
    return;
  }

  out.totalAccel = accel;
  out.totalGyro  = gyro;
  sLastAccel     = accel;
  sLastGyro      = gyro;
}

// Kept so nothing that only wants the two magnitudes has to know about the
// struct.
void readCrashMetrics(float &totalAccel, float &totalGyro) {
  MotionReading m;
  readMotion(m);
  totalAccel = m.totalAccel;
  totalGyro  = m.totalGyro;
}

int readBatteryPercent() {
  int raw = analogRead(BATTERY_PIN);
  float pinVoltage = (raw / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_DIVIDER_RATIO;
  float percent = (batteryVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0;
  return (int) constrain(percent, 0.0f, 100.0f);
}

#include "sensors.h"
#include "config.h"

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

static Adafruit_MPU6050 mpu;

// Set only once the sensor has actually answered on the bus. Everything else
// in this file reads it before touching I2C.
static bool sMpuPresent = false;

bool motionSensorPresent() {
  return sMpuPresent;
}

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

    // In the field, a unit that cannot detect a crash must not run as though
    // it can. Halting is the only honest thing it can do, and it is loud.
    if (DEPLOYMENT_MODE) {
      Serial.println();
      Serial.println("DEPLOYMENT MODE: halting. This unit cannot detect a crash,");
      Serial.println("and pretending otherwise would be worse than being dead.");
      while (1) {
        delay(1000);
      }
    }

    // On the bench that same halt stops everything else being tested - the
    // modem, the GPS, the audio - over one sensor that may be days away in the
    // post. So carry on, and make it impossible to forget why.
    Serial.println();
    Serial.println("**********************************************************");
    Serial.println("*  CRASH DETECTION IS OFF - the motion sensor is missing. *");
    Serial.println("*  Startup continues so the other modules can be brought  *");
    Serial.println("*  up on the bench. Every telemetry block will repeat it. *");
    Serial.println("*  DO NOT give this unit to a rider in this state.        *");
    Serial.println("**********************************************************");
    Serial.println();
    return;
  }

  sMpuPresent = true;
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
  // No sensor on the bus. Do NOT call getEvent(): each call would sit through
  // an I2C timeout, ten times a second, and drag the whole loop - and with it
  // the button, the mic and the telemetry - to a crawl.
  //
  // Reported as a resting value flagged substituted, which is a shape the rest
  // of the firmware already handles correctly: telemetry prints the read
  // failure, voiceMayCountNow() refuses to trust the microphone, and the crash
  // thresholds are never crossed, so nothing is falsely reported.
  if (!sMpuPresent) {
    out.ax = 0.0f;  out.ay = 0.0f;  out.az = 9.81f;
    out.gx = 0.0f;  out.gy = 0.0f;  out.gz = 0.0f;
    out.tempC       = 0.0f;
    out.totalAccel  = 9.81f;
    out.totalGyro   = 0.0f;
    out.substituted = true;
    return;
  }

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
  // No divider fitted means GPIO 34 is floating, and reading a floating
  // input-only pin produces drift, not a measurement. See config.h.
  if (!BATTERY_SENSE_FITTED) {
    return BATTERY_UNFITTED_REPORT;
  }

  int raw = analogRead(BATTERY_PIN);
  float pinVoltage = (raw / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * BATTERY_DIVIDER_RATIO;
  float percent = (batteryVoltage - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0;
  return (int) constrain(percent, 0.0f, 100.0f);
}

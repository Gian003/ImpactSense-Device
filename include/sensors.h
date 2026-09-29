#pragma once

// MPU6050 crash detection + battery ADC reading.

// One full sample from the MPU6050.
//
// readCrashMetrics() only ever handed back the two magnitudes, so the per-axis
// figures were computed every cycle and thrown away - which is why the serial
// output could only ever be two bare numbers. Nothing about the detection
// logic changes; this just stops discarding what was already measured.
struct MotionReading {
  float ax, ay, az;     // acceleration per axis, m/s^2
  float gx, gy, gz;     // rotation per axis, deg/s
  float totalAccel;     // magnitude, m/s^2 (~9.8 at rest - that is gravity)
  float totalGyro;      // magnitude, deg/s
  float tempC;          // MPU6050 die temperature

  // True when the I2C read looked bad and the last known-good magnitudes were
  // substituted. Worth surfacing: it means the bus is flaking, which otherwise
  // happens completely silently.
  bool  substituted;
};

void sensorsInit();
void readMotion(MotionReading &out);
void readCrashMetrics(float &totalAccel, float &totalGyro);
int readBatteryPercent();

// Whether the MPU6050 actually answered at boot.
//
// False means this unit CANNOT DETECT A CRASH. Off the bench, sensorsInit()
// halts rather than let that happen; on the bench it carries on so the modem,
// the GPS and the audio can be brought up while a replacement sensor is in the
// post. Anything that reports device health should say so plainly.
bool motionSensorPresent();

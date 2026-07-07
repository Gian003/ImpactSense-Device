#pragma once

// MPU6050 crash detection + battery ADC reading.

void sensorsInit();
void readCrashMetrics(float &totalAccel, float &totalGyro);
int readBatteryPercent();

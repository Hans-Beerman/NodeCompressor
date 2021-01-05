#include "TempSensor.h"
#include "OledDisplay.h"
#include <OneWire.h> 
#include <ACNode.h>

#ifndef TEMPSENSOR
#define TEMPSENSOR      ( 4)  // one wire digital input (GPIO4)
#endif

// One Wire input port for temperature sensor
#define ONE_WIRE_BUS (TEMPSENSOR)

#define TEMP_RESOLUTION (12) // 9, 10, 11 or 12 bit resolution of the ADC in the temperature sensor
#define MAX_TEMP_CONVERSIONTIME (750) // in ms
#define MAX_NR_OF_TRIES 3

#define MAX_TEMP_IS_TOO_HIGH_WINDOW (10000) // in ms default 10000 = 10 seconds. Error is only signalled after this time window is passed

// temperature sensor
OneWire oneWire(ONE_WIRE_BUS); // used for the temperature sensor
DallasTemperature sensorTemp(&oneWire);

int currentTempSensor = 0;

TemperatureSensor::TemperatureSensor(float tempIsHighLevel, float tempIsTooHighLevel, const char *tempLabel) {
  tempSensorNr = currentTempSensor++;
  theTempIsHighLevel = tempIsHighLevel;
	theTempIsTooHighLevel = tempIsTooHighLevel;
  conversionTime = MAX_TEMP_CONVERSIONTIME / (1 << (12 - TEMP_RESOLUTION));
  sprintf(labelTempSensor, "%s", tempLabel);
}

void TemperatureSensor::begin() {
  tempIsHigh = false;
  ErrorTempIsTooHigh = false;
  if (tempSensorNr == 0) {
    sensorTemp.begin();
  }

  tempSensorAvailable = sensorTemp.getAddress(tempDeviceAddress, tempSensorNr);
  if (!tempSensorAvailable) {
    temperature = -127;

    Log.print("Temperature sensor ");
    Log.print(tempSensorNr + 1);
    Log.print(" (");
    Log.print(labelTempSensor);
    Log.println("): sensor not detected at init of node!");
    return;
  }
  sensorTemp.setResolution(tempDeviceAddress, TEMP_RESOLUTION);
  sensorTemp.setWaitForConversion(false);
  sensorTemp.requestTemperaturesByAddress(tempDeviceAddress);
  tempAvailableStartTime = millis();
  tryCount = MAX_NR_OF_TRIES;
}

void TemperatureSensor::loop() {
  if (!tempSensorAvailable) {
    return;
  }
  if ((millis() - tempAvailableStartTime) > conversionTime) {
    currentTemperature = sensorTemp.getTempC(tempDeviceAddress);
    if (currentTemperature == -127) {
      currentTemperature = sensorTemp.getTempC(tempDeviceAddress);
    }
    if (currentTemperature == -127) {
      if (tryCount > 0) {
        tryCount--;
        tempAvailableStartTime = millis();
        return;
      } else {
        Log.print("Temperature sensor ");
        Log.print(tempSensorNr + 1);
        Log.print(" (");
        Log.print(labelTempSensor);
        Log.println("): sensor does not react, perhaps not available?");
      }
    } else {
      if (currentTemperature != previousTemperature) {
        previousTemperature = currentTemperature;
        temperature = currentTemperature;
/*        
        Serial.print("Temperature ");
        Serial.print(tempSensor + 1);
        Serial.print(" changed, current temperature = ");
        Serial.print(temperature);
        Serial.println(" degrees C");
*/        
      }
    }
    tryCount = MAX_NR_OF_TRIES;
    sensorTemp.requestTemperaturesByAddress(tempDeviceAddress);
    tempAvailableStartTime = millis();
    if (temperature <= theTempIsHighLevel) {
      if (tempIsHigh) {
        Log.print("Temperature sensor ");
        Log.print(tempSensorNr + 1);
        Log.print(" (");
        Log.print(labelTempSensor);
        Log.println("): temperature is OK now (below warning threshold)");
      }
      tempIsHigh = false;
      if (ErrorTempIsTooHigh)
      {
        nextTimeDisplay = true;
      }
      ErrorTempIsTooHigh = false;
      tempIsTooHighStartTime = 0;
    } else {
      if (!tempIsHigh) {
        Log.print("WARNING: temperature sensor ");
        Log.print(tempSensorNr + 1);
        Log.print(" (");
        Log.print(labelTempSensor);
        Log.println("): temperature is above warning level. Please check the compressor");
      }
      tempIsHigh = true;
      if ((temperature > theTempIsTooHighLevel) && !ErrorTempIsTooHigh) {
        if (tempIsTooHighStartTime == 0) {
          tempIsTooHighStartTime = millis();
        } else {
          if ((millis() - tempIsTooHighStartTime) > MAX_TEMP_IS_TOO_HIGH_WINDOW) {
            nextTimeDisplay = true;
            ErrorTempIsTooHigh = true;
            Log.print("ERROR, sensor ");
            Log.print(tempSensorNr + 1);
            Log.print(" (");
            Log.print(labelTempSensor);
            Log.println("): Temperature is too high, compressor is disabled. Please check the compressor!");
          }
        }
      } else {
        if ((temperature <= theTempIsTooHighLevel) && ErrorTempIsTooHigh) {
          tempIsTooHighStartTime = 0;
          ErrorTempIsTooHigh = false;
          nextTimeDisplay = true;
          Log.print("WARNING, sensor ");
          Log.print(tempSensorNr + 1);
          Log.print(" (");
          Log.print(labelTempSensor);
          Log.println("): Temperature is below error level now, but still above warning level. Please check the compressor!");
        }
      }
    }
  }
}


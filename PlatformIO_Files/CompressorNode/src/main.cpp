/*
      Copyright 2015-2018 Dirk-Willem van Gulik <dirkx@webweaving.org>
      Copyright 2020      Hans Beerman <hans.beerman@xs4all.nl>
                          Stichting Makerspace Leiden, the Netherlands.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef ESP32
#error "The CompressorNode uses an Olimex ESP32-PoE board only!"
#endif

#define ESP32_PoE
// #define OB_POLLING

// The GPIO pins used on the Olimax ESP32PoE
#define RELAY_GPIO                (14)  // digital output
#define ON_BUTTON                 (15)  // digital input
#define OFF_BUTTON                ( 5)  // digital input
#define OPTO1                     (36)  // digital input
#define LED1                      (32)  // digital output
#define LED2                      (33)  // digital output
#define OILLEVELSENSOR            (39)  // digital input
#define PRESSURESENSOR            (35)  // analog input
#define TEMPSENSOR                ( 4)  // one wire digital input
#define INFO_CALIBRATION_BUTTON   (34)  // this is the same as the CLEAR_EEPROM_AND_CACHE_BUTTON, BUT1 of the ESP32-PoE
                                        // pressing this button toggles Info / Calibration mode on of off. If on, IP address
                                        // and calibration information will be logged via MQTT and telnet

#include <Arduino.h>
#include <MachState.h>
#include <ACNode.h>
#include <WiredEthernet.h>
#include <SIG2.h>
#include <Cache.h>
#include <OptoDebounce.h>
#include <ButtonDebounce.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <NTP.h> // install NTP by Stefan Staub
//
// information about NTP.h, see: https://platformio.org/lib/show/5438/NTP
//
#include "TempSensor.h"
#include "PressureSensor.h"
#include "OledDisplay.h"
#include "OilLevelSensor.h"
#include ".passwd.h"
/* If using WiFi instead of a fixed ethernet connection, and/or OTA:

Add .passwd.h to the NodeCompressor/src directoy containing the following information

#pragma once

#define WIFI_NETWORK "YOUR_SSID"
#define WIFI_PASSWD "YOUR_PASSWD"

#define OTA_PASSWD "YOUR_OTA_PASSWD"
*/

WiFiUDP wifiUDP;
NTP ntp(wifiUDP);

// for test
#define MACHINE "bluecompressor"

// button on and button off
#define BUTTON_ON_PRESSED (LOW) // the input level of the GPIO port used for button on, if this button is pressed
#define BUTTON_OFF_PRESSED (LOW) // the input level of te GPIO port used for button off, if this button is pressed
#define BUTTON_INFO_CALIBRATION_PRESSED (LOW) // the input level of te GPIO port used for button info and calibrations, if this button is pressed

ButtonDebounce buttonOn(ON_BUTTON, 150 /* mSeconds */); // buttonOn is used to switch on the compressor
ButtonDebounce buttonOff(OFF_BUTTON, 150 /* mSeconds */); // buttonOff is used to switch off the compressor
ButtonDebounce buttonInfoCalibration(INFO_CALIBRATION_BUTTON, 150 /* mSeconds */); // buttonInfoCalibration is used to toggle Info / Calibration mode on/off

// 230VAC optocoupler
OptoDebounce opto1(OPTO1); // wired to N0 - L1 of 3 phase compressor motor, to detect if the motor has power (or not)

// temperature sensors
#define TEMP_SENSOR_LABEL1 ("Compressor") // label used in logging for temp. sensor 1
#define TEMP_SENSOR_LABEL2 ("Motor") // label used in logging for temp. sensor 2
#define TEMP_REPORT_ERROR1 ("temperature_sensor_1_(compressor)_error") // label used in reporting for temp. sensor 1
#define TEMP_REPORT_WARNING1 ("temperature_sensor_1_(compressor)_warning") // label used in reporting for temp. sensor 1
#define TEMP_REPORT1 ("temperature_sensor_1_(compressor)") // label used in reporting for temp. sensor 1
#define TEMP_REPORT_ERROR2 ("temperature_sensor_2_(motor)_error") // label used in reporting for temp. sensor 2
#define TEMP_REPORT_WARNING2 ("temperature_sensor_2_(motor)_warning") // label used in reporting for temp. sensor 2
#define TEMP_REPORT2 ("temperature_sensor_2_(motor)") // label used in reporting for temp. sensor 2
// Temperature warning and error levels
#define TEMP_IS_HIGH_LEVEL_1 (75.0) // in degrees Celcius, used for temperature is high warning of sensor 1
#define TEMP_IS_TOO_HIGH_LEVEL_1 (90.0) // in degrees Celcius, used to disable the compressor when temperature is too high of sensor 1
#define TEMP_IS_HIGH_LEVEL_2 (70.0) // in degrees Celcius, used for temperature is high warning of sensor 2
#define TEMP_IS_TOO_HIGH_LEVEL_2 (90.0) // in degrees Celcius, used to disable the compressor when temperature is too high of sensor 2


// Pressure limits, used for safety
#define PRESSURE_MAX_LIMIT (12.0)   // Compressor must be switched off above this limit for safety
#define PRESSURE_BELOW_LIMIT (10.0) // If compressor was switched off because pressure was to high, 
                                    // compressor can be switched on again if pressure becomes below this limit

// NTP update window
#define NTP_UPDATE_WINDOW (1000) // in ms


// setting PWM properties for LED's
#define PWM_FREQ (5000)
#define PWM_LED_CHANNEL1 (0)
#define PWM_LED_CHANNEL2 (1)
#define PWM_RESOLUTION (8)
#define LED1_DIM_VALUE (50) // 0 - 255, 0 = LED1 is off
#define LED2_DIM_VALUE (50) // 0 - 255, 0 = LED2 is off

// For LED's showing node error
#define BLINKING_LED_PERIOD (600) // in ms

// for storage in EEProm of the duration counters
#define SAVE_DURATION_COUNTERS_WINDOW (86400) // in seconds (86400 = 24 hour)

#define DURATION_DIR_PREFIX "/init"
#define DURATION_FILE_PREFIX "/duration"

// for auto switch off of the compressor
#define AUTOTIMEOUT (60 * 60 * 1000) // default: in ms 60 * 60 * 1000 = 60 minutes

// Compressor disabled (or not) at late hours:
// IF the compressor is not allowed at late hours (DISABLE_COMPRESSOR AT LATE HOURS = true) the 
// compressor will not switch on automatically (or by hand if the on button is pressed normally) from
// DISABLED_TIME_START to DISABLED_START_END
// Pressing the on button longer than MAX_WAIT_TIME_BUTTON_ON_PRESSED will override this behaviour by
// switching on the compressor anyhow. In all cases the compressor will switch of after AUTOTIMEOUT (in ms)
// unless the button on is pressed again or a new auto on command is received while the compressor is
// already switched on. In both cases the time will be extended by AUTOTIMEOUT ms.
#define DISABLE_COMPRESSOR_AT_LATE_HOURS      (true)
#define DISABLED_TIME_START                   (19) // in hour, time from which the compressor is not automatically switched on
#define DISABLED_TIME_END                     (7) // in hour, time to which the compressor is not automatically switched on
#define MAX_WAIT_TIME_BUTTON_ON_PRESSED       (10000)  // in ms, time button on must be pressed to override late hour compressor disable
#define MAX_WAIT_TIME_BUTTON_OFF_PRESSED      (5000)  // in ms, time button off must be pressed to switch compressor off immediately
#define LED_DISABLE_DURATION                  (5000)  // in ms, the time LED1 will flash if button on is pressed during late hour
#define LED_DISABLE_PERIOD                    (200)  // in ms, the time LED1 will flash on/off

// time window for calibration buttons
#define CALIB_WINDOW_TIME                     (5000) // in ms

// Clear EEProm button
// Press BUT1 on Olimex ESP32 PoE module before (re)boot of node
// keep BUT1 pressed for at least 5 s
// After the release of BUT1 node will restart with empty EEProm
#define CLEAR_EEPROM_AND_CACHE_BUTTON         (34)
#define CLEAR_EEPROM_AND_CACHE_BUTTON_PRESSED (LOW)

#define MAX_WAIT_TIME_BUTTON_PRESSED          (4000)  // in ms

// for logging to MQTT etc.
#define LOGGING_ENABLED                       (true)  // to enable/disable logging
#define LOGGING_TIME_WINDOW                   (20000)  // in ms


// for testing the timing of the different loops etc.
// #define TEST_TIMING
#define TEST_TIME_LIMIT (100)

#ifdef WIFI_NETWORK
ACNode node = ACNode(MACHINE, WIFI_NETWORK, WIFI_PASSWD);
#else
ACNode node = ACNode(MACHINE);
#endif

MqttLogStream mqttlogStream = MqttLogStream();
TelnetSerialStream telnetSerialStream = TelnetSerialStream();

#ifdef OTA_PASSWD
OTA ota = OTA(OTA_PASSWD);
#endif

#define NEVER (0)

struct {
  const char * label;                   // name of this state
  LED::led_state_t ledState;            // flashing pattern for the aartLED. Zie ook https://wiki.makerspaceleiden.nl/mediawiki/index.php/Powernode_1.1.
  time_t maxTimeInMilliSeconds;         // how long we can stay in this state before we timeout.
  machinestates_t failStateOnTimeout;   // what state we transition to on timeout.
} state[RUNNING + 1] =
{
  { "Booting",              LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Out of order",         LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Rebooting",            LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Transient Error",      LED::LED_ERROR,           120 * 1000, REBOOT },
  { "No network",           LED::LED_FLASH,           120 * 1000, REBOOT },
  { "Waiting for card",     LED::LED_IDLE,            NEVER, REBOOT },
  { "Checking card",        LED::LED_IDLE,            NEVER, REBOOT },
  { "Compressor switched off", LED::LED_IDLE,         NEVER, SWITCHEDOFF},
  { "Powered - motor off",  LED::LED_IDLE,            NEVER, POWERED },
  { "Powered - motor running", LED::LED_ON,           NEVER, RUNNING },
};

unsigned long laststatechange = 0;
static machinestates_t laststate = BOOTING;
machinestates_t machinestate = BOOTING;

unsigned long powered_total = 0, powered_last;
unsigned long running_total = 0, running_last;

float powered = 0.0;
float running = 0.0;

unsigned long lastSavedPoweredCounter = 0;
unsigned long lastSavedRunningCounter = 0;

// pressure sensor
PressureSensor thePressureSensor(PRESSURE_MAX_LIMIT, PRESSURE_BELOW_LIMIT);

// temperature sensors
TemperatureSensor theTempSensor1(TEMP_IS_HIGH_LEVEL_1, TEMP_IS_TOO_HIGH_LEVEL_1, TEMP_SENSOR_LABEL1);
TemperatureSensor theTempSensor2(TEMP_IS_HIGH_LEVEL_2, TEMP_IS_TOO_HIGH_LEVEL_2, TEMP_SENSOR_LABEL2);

// oil level sensor
OilLevelSensor theOilLevelSensor;

// OledDisplay
OledDisplay theOledDisplay;

unsigned long blinkingLedNextTime = 0;
bool blinkingLedIsOn = false;
bool ledIsBlinking = false;

char reportStr[128];

char ntpDateTimeStr[64];

unsigned long nextLoggingTime = 0;

unsigned long DurationCounterSave;

unsigned long verifyButtonOnPressedTime;
unsigned long verifyButtonOffPressedTime;
bool verifyButtonOnIsStillPressed = false;
bool verifyButtonOffIsStillPressed = false;
bool isManualSwitchedOn = false;
bool isManualSwitchedOnVerifyOverride = false;
bool isManualSwitchedOffVerifyOverride = false;
bool isManualSwitchedOff = false;
bool isManualSwitchedOffWait = false;
bool isManualTimeOutExtended = false;

unsigned long ledDisableTime = 0;
unsigned long nextLedDisableTime = 0;
bool showLedDisable = false;
bool disableLedIsOn = false;

unsigned long autoPowerOff;
bool compressorIsOn = false;
bool CompressorMustBeSwitchedOff = false;

bool automaticStopReceived = false;
bool automaticPowerOnReceived = false;
bool automaticPowerOnDenied = false;

bool ErrorPressureIsTooHigh = false;
bool showErrorPressureIsTooHigh = false;
bool showPressureIsOK = false;

bool checkCalibButtonsPressed = false;
long int checkCalibTimeOut = 0;
bool showInfoAndCalibration = false;
IPAddress theLocalIPAddress;

unsigned long nextNTPUpdateTime = 0;


// for testing loop timing
long long prevTime = 0;
int waitcount = 0;
int placeCount = 0;
long long averageTime[40] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,};

void checkClearEEPromAndCacheButtonPressed(void) {
  unsigned long ButtonPressedTime;
  unsigned long prevSecs;
  bool firstTime = true;

  // check CLEAR_EEPROM_AND_CACHE_BUTTON pressed
  pinMode(CLEAR_EEPROM_AND_CACHE_BUTTON, INPUT);
  // check if button is pressed for at least 4 s
  Log.println("Checking if the button is pressed for clearing EEProm and cache");

	
  if (digitalRead(CLEAR_EEPROM_AND_CACHE_BUTTON) == CLEAR_EEPROM_AND_CACHE_BUTTON_PRESSED) {
    theOledDisplay.clearEEPromWarning();
    ButtonPressedTime = millis();  
    prevSecs = MAX_WAIT_TIME_BUTTON_PRESSED / 1000;
    Log.print("Keep the button pressed at least: ");
    Log.print(prevSecs);
    Log.println(" s");
    while (digitalRead(CLEAR_EEPROM_AND_CACHE_BUTTON) == CLEAR_EEPROM_AND_CACHE_BUTTON_PRESSED) {
      if ((millis() - ButtonPressedTime) >= MAX_WAIT_TIME_BUTTON_PRESSED) {
        if (firstTime) {
          Log.println("");
          Log.println("Please release the button");
          theOledDisplay.showStatus(RELEASEBUTTON);
          firstTime = false;
        }
      }
    }

    if ((millis() - ButtonPressedTime) >= MAX_WAIT_TIME_BUTTON_PRESSED) {
      Log.println("");
      Log.print("Button for clearing EEProm and cache was pressed for more than ");
      Log.print(MAX_WAIT_TIME_BUTTON_PRESSED / 1000);
      Log.println(" s, EEProm and Cache will be cleared!");
      theOledDisplay.clearEEPromMessage();
      theOledDisplay.showStatus(NOSTATUS);
      // Clear EEPROM
      EEPROM.begin(1024);
      wipe_eeprom();
      Log.println("EEProm cleared!");
      theOledDisplay.EEPromCleared();      
      // Clear cache
      prepareCache(true);
      // Clear duration counter file
      String path = DURATION_DIR_PREFIX + (String)DURATION_FILE_PREFIX;
      if (SPIFFS.exists(path)) {
        SPIFFS.remove(path);
      } 
      theOledDisplay.cacheCleared();
      Log.println("Cache cleared!");
      // wait until button is released, than reboot
      while (digitalRead(CLEAR_EEPROM_AND_CACHE_BUTTON) == CLEAR_EEPROM_AND_CACHE_BUTTON_PRESSED) {
        // do nothing here
      }
      Log.println("Node will be restarted");
      theOledDisplay.showStatus(NODEREBOOT);
      // restart node
      ESP.restart();
    } else {
      Log.println("\rButton was not (or not long enough) pressed to clear EEProm and cache");
    }
  }
}

void saveDurationCounters() {
  unsigned long tmpCounter1;
  unsigned long tmpCounter2;

  if (machinestate >= POWERED) {
    tmpCounter1 = powered_total + (millis() - powered_last) / 1000;
  } else {
    tmpCounter1 = powered_total;
  }

  if (machinestate == RUNNING) {
    tmpCounter2 = running_total + (millis() - running_last) / 1000;
  } else {
    tmpCounter2 = running_total;
  }

  if ((tmpCounter1 != lastSavedPoweredCounter) || (tmpCounter2 != lastSavedRunningCounter)) {
    String path = DURATION_DIR_PREFIX + (String)DURATION_FILE_PREFIX;
    File durationFile;
    unsigned int writeSize;
    durationFile = SPIFFS.open(path, "wb");
    if(!durationFile) {
      Log.print("There was an error opening the ");
      Log.print(DURATION_DIR_PREFIX);
      Log.print(DURATION_FILE_PREFIX);
      Log.println(" file for writing");
      return;
    }
    writeSize = durationFile.write((byte*)&tmpCounter1, sizeof(tmpCounter1));
    if (writeSize != sizeof(tmpCounter1)) {
      Log.print("ERROR --> powered_total: ");
      Log.print(tmpCounter1);
      Log.println(" NOT stored in SPIFFS");
      durationFile.close();
      return;
    }

    writeSize = durationFile.write((byte*)&tmpCounter2, sizeof(tmpCounter2));
    if (writeSize != sizeof(tmpCounter2)) {
      Log.print("ERROR --> running_total: ");
      Log.print(tmpCounter2);
      Log.println(" NOT stored in SPIFFS");
      durationFile.close();
      return;
    }
    durationFile.close();
  }
}

void loadDurationCounters() {
  String path = DURATION_DIR_PREFIX + (String)DURATION_FILE_PREFIX;
  File durationFile;
  unsigned int readSize;

  if(!SPIFFS.begin(false)){
    Log.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  if (SPIFFS.exists(path)) {
    durationFile = SPIFFS.open(path, "rb");
    if(!durationFile) {
      Log.print("There was an error opening the ");
      Log.print(DURATION_DIR_PREFIX);
      Log.print(DURATION_FILE_PREFIX);
      Log.println(" file for reading");
      return;
    }
    durationFile.setTimeout(0);
    readSize = durationFile.readBytes((char*)&powered_total, sizeof(powered_total));
    if (readSize == sizeof(powered_total)) {
     powered = (float)powered_total / 3600.0;
    } else {
      Log.print("There was an error reading powered_total from ");
      Log.print(DURATION_DIR_PREFIX);
      Log.println(DURATION_FILE_PREFIX);
      durationFile.close();
      return;
    }
    readSize = durationFile.readBytes((char*)&running_total, sizeof(running_total));
    if (readSize == sizeof(running_total)) {
      running = (float)running_total / 3600.0;
    } else {
      Log.print("There was an error reading powered_total from ");
      Log.print(DURATION_DIR_PREFIX);
      Log.println(DURATION_FILE_PREFIX);
      durationFile.close();
      return;
    }
 
    durationFile.close();
    return;
  } else {
    Log.print("The file ");
    Log.print(DURATION_DIR_PREFIX);
    Log.print(DURATION_FILE_PREFIX);
    Log.println(" is not available!");
  }
}

bool compressorIsDisabeled() {
  int currentHour;

  currentHour = ntp.hours();
/* 
 * for test of NTP, check if time shown is correct 
  Serial.print("Current hour: ");
  Serial.println(currentHour);
*/  
  if (DISABLED_TIME_START < DISABLED_TIME_END) {
    if (ErrorOilLevelIsTooLow ||  theTempSensor1.ErrorTempIsTooHigh || theTempSensor2.ErrorTempIsTooHigh || ErrorPressureIsTooHigh || thePressureSensor.tooHighPressure() ||
        (((currentHour >= DISABLED_TIME_START) && (currentHour < DISABLED_TIME_END)) && DISABLE_COMPRESSOR_AT_LATE_HOURS)) {
      return true;
    } else {
      return false;
    }
  } else {
    if (ErrorOilLevelIsTooLow || theTempSensor1.ErrorTempIsTooHigh || theTempSensor2.ErrorTempIsTooHigh ||
        (((currentHour >= DISABLED_TIME_START) || (currentHour < DISABLED_TIME_END)) && DISABLE_COMPRESSOR_AT_LATE_HOURS)) {
      return true;
    } else {
      return false;
    }
  }
}

void buttonOnChanged(int state) {
  // Debug.printf("Button On changed to %d\n", state);
  if (machinestate != SWITCHEDOFF && CompressorMustBeSwitchedOff) {
    CompressorMustBeSwitchedOff = false;
    isManualSwitchedOff = false;
    isManualSwitchedOffWait = false;
    autoPowerOff = millis();
    isManualSwitchedOn = true;
    return;
  }

  if ((state == BUTTON_ON_PRESSED) && !ErrorOilLevelIsTooLow && !theTempSensor1.ErrorTempIsTooHigh && !theTempSensor2.ErrorTempIsTooHigh 
        && !ErrorPressureIsTooHigh && thePressureSensor.lowPressure()
        && (buttonOff.state() != BUTTON_OFF_PRESSED) && (machinestate == SWITCHEDOFF)) {
    if (!compressorIsDisabeled()) {
      digitalWrite(RELAY_GPIO, 1);
      // digitalWrite(LED1, 1);   
      ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);   
      machinestate = POWERED;
      compressorIsOn = true;
      autoPowerOff = millis();
      isManualSwitchedOn = true;
      verifyButtonOnIsStillPressed = false;
    } else {
      verifyButtonOnPressedTime = millis();
      isManualSwitchedOnVerifyOverride = true;
      // flash LED to show that function is disabled
      ledDisableTime = millis();
      nextLedDisableTime = millis();
      showLedDisable = true;
      // digitalWrite(LED1, 1);
      ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
      disableLedIsOn = true;
      verifyButtonOnIsStillPressed = true;
    }
  } else {
    if (ErrorPressureIsTooHigh || !thePressureSensor.lowPressure()) {
      showErrorPressureIsTooHigh = true;
    }
    if ((state == BUTTON_ON_PRESSED) && (machinestate > SWITCHEDOFF)) {
      autoPowerOff = millis();
      isManualTimeOutExtended = true;
    }
    verifyButtonOnIsStillPressed = false;

    if ((state == BUTTON_ON_PRESSED) && (digitalRead(OFF_BUTTON) == BUTTON_OFF_PRESSED)) {
      if (showInfoAndCalibration) {
        showInfoAndCalibration = false;
      } else {
        checkCalibButtonsPressed = true;
        checkCalibTimeOut = millis();
      }
    } else {
      if (!(state == BUTTON_ON_PRESSED)) {
        checkCalibButtonsPressed = false;
      }
    }
  }
}

void buttonOffChanged(int state) {
//    Debug.printf("Button Off changed to %d\n", state);
  if ((state == BUTTON_OFF_PRESSED) && (buttonOn.state() != BUTTON_ON_PRESSED) && ((machinestate >= POWERED) || ErrorPressureIsTooHigh)) {
    if ((opto1.state() == OptoDebounce::OFF) || ErrorPressureIsTooHigh) {
      CompressorMustBeSwitchedOff = true;
  //    digitalWrite(RELAY_GPIO, 0);
      // digitalWrite(LED1, 0);
      // digitalWrite(LED2, 0);
      // ledcWrite(PWM_LED_CHANNEL1, 0);
      // ledcWrite(PWM_LED_CHANNEL2, 0);
      // compressorIsOn = false;
      // machinestate = SWITCHEDOFF;
      isManualSwitchedOff = true;
      if (ErrorPressureIsTooHigh) {
        showErrorPressureIsTooHigh = true;
      }
    } else {
      verifyButtonOffPressedTime = millis();
      isManualSwitchedOffWait = true;
      isManualSwitchedOffVerifyOverride = true;
      // flash LED to show that function is disabled for a while
      ledDisableTime = millis();
      nextLedDisableTime = millis();
      showLedDisable = true;
      // digitalWrite(LED1, 1);
      ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
      disableLedIsOn = true;
      verifyButtonOffIsStillPressed = true;
    }
  } else {
    if ((state == BUTTON_OFF_PRESSED) && (digitalRead(ON_BUTTON) == BUTTON_ON_PRESSED)) {
      if (showInfoAndCalibration) {
        showInfoAndCalibration = false;
      } else {
        checkCalibButtonsPressed = true;
        checkCalibTimeOut = millis();
      }
    } else {
      if (!(state == BUTTON_OFF_PRESSED)) {
        checkCalibButtonsPressed = false;
      }
    }
  }
}


void setup() {
  Serial.begin(115200);
  Serial.println("\n\n\n");
  Serial.println("Booted: " __FILE__ " " __DATE__ " " __TIME__ );

  // Init the hardware and get it into a safe state.
  //
  pinMode(RELAY_GPIO, OUTPUT);
  digitalWrite(RELAY_GPIO, 0);

  pinMode(ON_BUTTON, INPUT_PULLUP);
  pinMode(OFF_BUTTON, INPUT_PULLUP);

  /*
  pinMode(LED1, OUTPUT);
  digitalWrite(LED1, 0);
  pinMode(LED2, OUTPUT);
  digitalWrite(LED2, 0);
  */

  // configure LED PWM functionalitites for dimming the LED's
  ledcSetup(PWM_LED_CHANNEL1, PWM_FREQ, PWM_RESOLUTION);
  ledcSetup(PWM_LED_CHANNEL2, PWM_FREQ, PWM_RESOLUTION);

  // attach the channels to the GPIO's to be controlled
  ledcAttachPin(LED1, PWM_LED_CHANNEL1);
  ledcAttachPin(LED2, PWM_LED_CHANNEL2);

  theOledDisplay.begin(TEMP_IS_HIGH_LEVEL_1, TEMP_IS_TOO_HIGH_LEVEL_1, TEMP_IS_HIGH_LEVEL_2, TEMP_IS_TOO_HIGH_LEVEL_2);

  Serial.printf("Boot state: ButtonOn:%d ButtonOff:%d\n", digitalRead(ON_BUTTON), digitalRead(OFF_BUTTON));

  checkClearEEPromAndCacheButtonPressed();

  loadDurationCounters();
  DurationCounterSave = millis() / 1000;

  node.set_mqtt_prefix("ac");
  node.set_master("master");

  // the default is space.makerspaceleiden.nl, prefix test
  // node.set_mqtt_host("mymqtt-server.athome.nl");
  // node.set_mqtt_prefix("test-1234");

  // specify this when using your own `master'.
  //
  // node.set_master("test-master");

  // node.set_report_period(2 * 1000);

  buttonOn.setCallback(buttonOnChanged);

  buttonOff.setCallback(buttonOffChanged);

  buttonInfoCalibration.setCallback([](int state) {
    showInfoAndCalibration = !showInfoAndCalibration;
    saveDurationCounters();
  });

  theOilLevelSensor.begin();

  node.onConnect([]() {
    machinestate = SWITCHEDOFF;
  });
  node.onDisconnect([]() {
    machinestate = NOCONN;
  });
  node.onError([](acnode_error_t err) {
    Log.print("Error ");
    Log.println(err);
    machinestate = TRANSIENTERROR;
  });

  node.onValidatedCmd([](const char *cmd, const char * rest) -> ACBase::cmd_result_t  {
    if (!strcasecmp(cmd, "stop")) {
      // machinestate = SWITCHEDOFF;
      CompressorMustBeSwitchedOff = true;
      // digitalWrite(RELAY_GPIO, 0);
      // digitalWrite(LED1, 0);
      // digitalWrite(LED2, 0);
      // ledcWrite(PWM_LED_CHANNEL1, 0);
      // ledcWrite(PWM_LED_CHANNEL2, 0);
      // compressorIsOn = false;
      automaticStopReceived = true;
      // ErrorPressureIsTooHigh = false;
      return ACNode::CMD_CLAIMED;
    };

    if (!strcasecmp(cmd, "poweron")) {
      if (!compressorIsDisabeled()) {
        if (machinestate < POWERED) {
          digitalWrite(RELAY_GPIO, 1);
          // digitalWrite(LED1, 1);
          // digitalWrite(LED2, 0);
          ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
          ledcWrite(PWM_LED_CHANNEL2, 0);
          compressorIsOn = true;
          machinestate = POWERED;
          automaticPowerOnReceived = true;
        };
        autoPowerOff = millis();
      } else {
        automaticPowerOnDenied = true;
      }
      return ACBase::CMD_CLAIMED;
    };
    return ACBase::CMD_DECLINE;
  });

  node.onReport([](JsonObject  & report) {
    report["state"] = state[machinestate].label;

    powered = ((float)powered_total + ((machinestate == POWERED) ? (float)((millis() - powered_last) / 1000) : 0)) / 3600;
    running = ((float)running_total + ((machinestate == RUNNING) ? (float)((millis() - running_last) / 1000) : 0)) / 3600;

    sprintf(reportStr, "%f hours", powered);
    report["powered_time"] = reportStr;
    sprintf(reportStr, "%f hours", running);
    report["running_time"] = reportStr;

    if (theTempSensor1.temperature == -127) {
      sprintf(reportStr, "ERROR: reading temp. sens. connected?");
      report[TEMP_SENSOR_LABEL1] = reportStr;
    } else {
      if (theTempSensor1.ErrorTempIsTooHigh) {
        sprintf(reportStr, "ERROR: Temp. is too high, compr. disabled!");
        report[TEMP_SENSOR_LABEL1] = reportStr;
      } else {
        if (theTempSensor1.tempIsHigh) {
          sprintf(reportStr, "WARNING: Temp. is very high!");
          report[TEMP_SENSOR_LABEL1] = reportStr;
        }
      }
      sprintf(reportStr, "Temp: %6.2f degr. C", theTempSensor1.temperature);
      report[TEMP_SENSOR_LABEL1] = reportStr;
    }
    

    if (theTempSensor2.temperature == -127) {
      sprintf(reportStr, "ERROR: reading temp, sens. connected?");
      report[TEMP_SENSOR_LABEL2] = reportStr;
    } else {
      if (theTempSensor2.ErrorTempIsTooHigh) {
        sprintf(reportStr, "ERROR: Temp. is too high, compr. disabled!");
        report[TEMP_SENSOR_LABEL2] = reportStr;
      } else {
        if (theTempSensor2.tempIsHigh) {
          sprintf(reportStr, "WARNING: Temp. is very high!");
          report[TEMP_SENSOR_LABEL2] = reportStr;
        }
      }
      sprintf(reportStr, "Temp: %6.2f degr. C", theTempSensor2.temperature);
      report[TEMP_SENSOR_LABEL2] = reportStr;
    }


    if (!oilLevelIsTooLow)
    {
      report["oil_level"] = "oil level is OK!";
    } else {
      if (ErrorOilLevelIsTooLow) {
        report["oil_level"] = "ERROR: Oil level too low, compressor disabled";
      } else {
        report["oil_level"] = "WARNING: Oil level low!";
      }
    }
    sprintf(reportStr, "%5.2f bar", pressure);
    report["pressure"] = reportStr;
#ifdef OTA_PASSWD
    report["ota"] = true;
#else
    report["ota"] = false;
#endif
//    report["opto1"] = opto1.state();
    report["boot"] = ntpDateTimeStr;
  });

  Log.addPrintStream(std::make_shared<MqttLogStream>(mqttlogStream));

  auto t = std::make_shared<TelnetSerialStream>(telnetSerialStream);
  Log.addPrintStream(t);
  Debug.addPrintStream(t);

  // init temperature sensors and start reading first values
  theTempSensor1.begin();
  theTempSensor2.begin();

#ifdef OTA_PASSWD
  node.addHandler(&ota);
#endif

  // node.set_debug(true);
  // node.set_debugAlive(true);

  // Olimex ESP32-PoE board is used
  node.begin(BOARD_OLIMEX);
  
  theOledDisplay.clearDisplay();

  ntp.ruleDST("CEST", Last, Sun, Mar, 2, 120); // last sunday in march 2:00, timezone +120min (+1 GMT + 1h summertime offset)
  ntp.ruleSTD("CET", Last, Sun, Oct, 3, 60); // last sunday in october 3:00, timezone +60min (+1 GMT)
  ntp.begin();
  ntp.update();

  int currentHour = ntp.hours();
  int currentMinutes = ntp.minutes();
  int currentSecs = ntp.seconds();
  int currentDay = ntp.day();
  int currentMonth = ntp.month();
  int currentYear = ntp.year();

  sprintf(ntpDateTimeStr, "Last boot: %4d-%02d-%02d %02d:%02d:%02d", currentYear, currentMonth, currentDay, currentHour, currentMinutes, currentSecs);

  Log.println("Booted: " __FILE__ " " __DATE__ " " __TIME__ );
  Log.println(ntpDateTimeStr);
}

void buttons_optocoupler_loop() {

  opto1.loop();

  if (opto1.state() == OptoDebounce::ON) {
    if (machinestate == POWERED) {
      // digitalWrite(LED2, 1);
      ledcWrite(PWM_LED_CHANNEL2, LED2_DIM_VALUE);
      machinestate = RUNNING;
    } 
  } else {
    if (machinestate == RUNNING) {
      // digitalWrite(LED2, 0);
      ledcWrite(PWM_LED_CHANNEL2, 0);
      machinestate = POWERED;        
    }
  }

  if (isManualSwitchedOn) {
    isManualSwitchedOn = false;
    Log.println("Compressor switched on with button");
    theOledDisplay.showStatus(MANUALSWITCHON);
  }

  if (isManualSwitchedOnVerifyOverride) {
    isManualSwitchedOnVerifyOverride = false;
    Log.println("Power on denied!");
    Log.println("Power on is disabled during evening/night window");
    theOledDisplay.showStatus(POWERONDISABLED);
  }

  if (isManualSwitchedOff) {
    isManualSwitchedOff = false;
    Log.println("Compressor switched off with button");
    theOledDisplay.showStatus(MANUALSWITCHOFF);
  }

  if (isManualSwitchedOffWait) {
    isManualSwitchedOffWait = false;
    Log.println("Compressor switch off initiated with button");
    theOledDisplay.showStatus(MANUALSWITCHOFFWAIT);
  }

  if (isManualTimeOutExtended) {
    isManualTimeOutExtended = false;
    Log.println("Compressor timeout extended with button");
    theOledDisplay.showStatus(TIMEOUTEXTENDED);
  }

  if (verifyButtonOnIsStillPressed) {
    if ((millis() - verifyButtonOnPressedTime) > MAX_WAIT_TIME_BUTTON_ON_PRESSED) {
      if (buttonOn.state() == BUTTON_ON_PRESSED) {
        verifyButtonOnIsStillPressed = false;
        digitalWrite(RELAY_GPIO, 1);
        // digitalWrite(LED1, 1);
        // digitalWrite(LED2, 0);
        ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
        ledcWrite(PWM_LED_CHANNEL2, 0);
        compressorIsOn = true;
        machinestate = POWERED;
        autoPowerOff = millis(); 
        theOledDisplay.showStatus(MANUALOVERRIDE);
        Log.println("Warning: compressor was switched on using manual override!");
      }
    } 
  }

  if (verifyButtonOffIsStillPressed) {
    if (buttonOff.state() == BUTTON_OFF_PRESSED) {
      if (((millis() - verifyButtonOffPressedTime) > MAX_WAIT_TIME_BUTTON_OFF_PRESSED) || (opto1.state() == OptoDebounce::OFF)) {
        CompressorMustBeSwitchedOff = false;
        verifyButtonOffIsStillPressed = false;
        digitalWrite(RELAY_GPIO, 0);
        // digitalWrite(LED1, 1);
        // digitalWrite(LED2, 0);
        ledcWrite(PWM_LED_CHANNEL1, 0);
        ledcWrite(PWM_LED_CHANNEL2, 0);
        compressorIsOn = false;
        machinestate = SWITCHEDOFF;
        theOledDisplay.showStatus(MANUALOVERRIDE);
        Log.println("Warning: compressor was switched off immediately using manual override!");
      }
    } else {
      showLedDisable = false;
      ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
      CompressorMustBeSwitchedOff = true;
      isManualSwitchedOffWait = true;
      verifyButtonOffIsStillPressed = false;
    }
  }


  if (showLedDisable) {
    if ((millis() - ledDisableTime) < LED_DISABLE_DURATION) {
      if ((millis() - nextLedDisableTime) >= LED_DISABLE_PERIOD) {
        nextLedDisableTime = millis();
        if (disableLedIsOn) {
          // digitalWrite(LED1, 0);
          ledcWrite(PWM_LED_CHANNEL1, 0);
        } else {
          // digitalWrite(LED1, 1);
          ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
        }
        disableLedIsOn = !disableLedIsOn;
      }
    } else {
      showLedDisable = false;
      disableLedIsOn = false;
      // digitalWrite(LED1, 0);
      ledcWrite(PWM_LED_CHANNEL1, 0);
    }
  }

  if (checkCalibButtonsPressed) {
    if ((buttonOn.state() == BUTTON_ON_PRESSED) && (buttonOff.state() == BUTTON_OFF_PRESSED)) {
      if ((millis() - checkCalibTimeOut) > CALIB_WINDOW_TIME) {
        showInfoAndCalibration = !showInfoAndCalibration;
        saveDurationCounters();
        checkCalibButtonsPressed = false;
      }
    } else {
      checkCalibButtonsPressed = false;
    }
  }
}

void compressorLoop() {
 
  if ((millis() - nextNTPUpdateTime) > NTP_UPDATE_WINDOW) {
    ntp.update();
    nextNTPUpdateTime = millis();
  }
  

  if (machinestate > SWITCHEDOFF) {
    // check if compressor must be automatically switched off
    if ((millis() - autoPowerOff) > AUTOTIMEOUT) {
      CompressorMustBeSwitchedOff = true;
    }
    
    // check if compressor must be switched off
    if (ErrorOilLevelIsTooLow || theTempSensor1.ErrorTempIsTooHigh || theTempSensor2.ErrorTempIsTooHigh || thePressureSensor.tooHighPressure() || (CompressorMustBeSwitchedOff && (opto1.state() == OptoDebounce::OFF))) {
      CompressorMustBeSwitchedOff = false;
      digitalWrite(RELAY_GPIO, 0);
      // digitalWrite(LED1, 0);
      // digitalWrite(LED2, 0);
      ledcWrite(PWM_LED_CHANNEL1, 0);
      ledcWrite(PWM_LED_CHANNEL2, 0);
      compressorIsOn = false;
      machinestate = SWITCHEDOFF;
      theOledDisplay.showStatus(NOSTATUS);

      if (ErrorOilLevelIsTooLow || theTempSensor1.ErrorTempIsTooHigh || theTempSensor2.ErrorTempIsTooHigh) {
        Log.println("Compressor is disabled now due to error(s). Please check compressor!");
      }
      if  (CompressorMustBeSwitchedOff && (opto1.state() == OptoDebounce::OFF)) {
        Log.println("Compressor is disabled now due to error(s). Please check compressor!");
      }
      if (thePressureSensor.tooHighPressure()) {
        ErrorPressureIsTooHigh = true;
        Log.print("Pressure is too high: ");
        Log.print(pressure);
        Log.println(" bar, compressor is switched off");
        theOledDisplay.showStatus(ERRORPRESSUREISTOOHIGH);
      }
      if ((millis() - autoPowerOff) > AUTOTIMEOUT) {
        Log.println("Timeout: compressor automatically switched off");
        theOledDisplay.showStatus(TIMEOUT);
      }
    }
  } else {
    if (ErrorOilLevelIsTooLow || theTempSensor1.ErrorTempIsTooHigh || theTempSensor2.ErrorTempIsTooHigh || !thePressureSensor.lowPressure()) {
      ledIsBlinking = true;
      if ((millis() - blinkingLedNextTime) > BLINKING_LED_PERIOD) {
        if (blinkingLedIsOn) {
          // digitalWrite(LED1, 0);
          // digitalWrite(LED2, 0);
          ledcWrite(PWM_LED_CHANNEL1, 0);
          ledcWrite(PWM_LED_CHANNEL2, 0);
        } else {
          // digitalWrite(LED1, 1);
          // digitalWrite(LED2, 1);
          ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
          ledcWrite(PWM_LED_CHANNEL2, LED2_DIM_VALUE);
        }
        blinkingLedIsOn = !blinkingLedIsOn;
        blinkingLedNextTime = millis();
      }
    } else {
      if (ErrorPressureIsTooHigh && thePressureSensor.lowPressure()) {
        ErrorPressureIsTooHigh = false;
        if ((millis() - autoPowerOff) > AUTOTIMEOUT) {
          machinestate = POWERED;
          theOledDisplay.showStatus(NOSTATUS);
        }
      }
    }
  }

  if (showErrorPressureIsTooHigh) {
    theOledDisplay.showStatus(ERRORPRESSUREISTOOHIGH);
    ErrorPressureIsTooHigh = true;
    showErrorPressureIsTooHigh = false;
  }

  if (showPressureIsOK) {
    ErrorPressureIsTooHigh = false;
    theOledDisplay.showStatus(NOSTATUS);
    showPressureIsOK = false;
  }
  
  if (automaticStopReceived) {
    Log.println("Automatic request received: Compressor stopped");
    theOledDisplay.showStatus(AUTOSWITCHOFF);
    automaticStopReceived = false;
  }
  if (automaticPowerOnReceived) {
    Log.println("Automatic request received: Compressor powered on");
    theOledDisplay.showStatus(AUTOSWITCHON);
    automaticPowerOnReceived = false;
  }  

  if (automaticPowerOnDenied) {
    Log.println("Automatic request denied to power on the compressor. Reason: late hours/night!");
    theOledDisplay.showStatus(AUTOONDENIED);
    automaticPowerOnDenied = false;
  }  

  if (ledIsBlinking) {
    if (!ErrorOilLevelIsTooLow && !theTempSensor1.ErrorTempIsTooHigh && !theTempSensor2.ErrorTempIsTooHigh && !ErrorPressureIsTooHigh) {
      // digitalWrite(LED1, 0);
      // digitalWrite(LED2, 0);
      ledcWrite(PWM_LED_CHANNEL1, 0);
      ledcWrite(PWM_LED_CHANNEL2, 0);
      ledIsBlinking = false;
    }
  }

  // save duration counters in EEProm every SAVE_DURATION_COUNTERS_WINDOW number of seconds
  if (((millis() / 1000) - DurationCounterSave) > SAVE_DURATION_COUNTERS_WINDOW) {
    DurationCounterSave = millis() / 1000;

    Log.print("powered_total = ");
    Log.println(powered_total);
    Log.print("running_total = ");
    Log.println(running_total);

    saveDurationCounters();
  }

  if (showInfoAndCalibration && thePressureSensor.newCalibrationInfoAvailable) {
    Log.println("Compressor Node Info");
    Log.print("Software version :");
    Log.println(SOFTWARE_VERSION);
    theLocalIPAddress = node.localIP();
    Log.print("IP address: ");
    Log.println(theLocalIPAddress.toString());
    thePressureSensor.logInfoCalibration();
    Log.println("");
  }

  if (LOGGING_ENABLED && ((millis() - nextLoggingTime) > LOGGING_TIME_WINDOW)) {
    nextLoggingTime = millis();
    
    Log.println("");

    // Log pressure
    Log.print("Pressure = ");
    Log.print(pressure);
    Log.println(" bar");

    // Log oil level
    if (ErrorOilLevelIsTooLow) {
      Log.println("ERROR: Oil level is too low; Compressor will be disabled; Please maintain the compressor by filling up the oil");
    } else {
      if (oilLevelIsTooLow) {
        Log.println("Warning: Oil level is too low; Compressor will be disabled soon if this issue is not solved; Please verify the oil level and fill up if needed");
      } else {
        Log.println("Oil level is OK!");
      }
    }

    // log temperature
    if (theTempSensor1.temperature == -127) {
      sprintf(reportStr, "Error reading temperature sensor 1 (%s), perhaps not connected?", TEMP_SENSOR_LABEL1);
    } else {
      if (theTempSensor1.ErrorTempIsTooHigh) {
        sprintf(reportStr, "ERROR: Temperature sensor 1 (%s) is too high, compressor is disabled!", TEMP_SENSOR_LABEL1);
        Log.println(reportStr);
      } else {
        if (theTempSensor1.tempIsHigh) {
          sprintf(reportStr, "WARNING: Temperature sensor 1 (%s) is very high!", TEMP_SENSOR_LABEL1);
          Log.println(reportStr);
        }
      }
      sprintf(reportStr, "Temperature sensor 1 (%s) = %f degrees Celcius", TEMP_SENSOR_LABEL1, theTempSensor1.temperature);
    }
    Log.println(reportStr);

    if (theTempSensor2.temperature == -127) {
      sprintf(reportStr, "Error reading temperature sensor 2 (%s), perhaps not connected?", TEMP_SENSOR_LABEL2);
    } else {
      if (theTempSensor2.ErrorTempIsTooHigh) {
        sprintf(reportStr, "ERROR: Temperature sensor 2 (%s) is too high, compressor is disabled!", TEMP_SENSOR_LABEL2);
        Log.println(reportStr);
      } else {
        if (theTempSensor2.tempIsHigh) {
          sprintf(reportStr, "WARNING: Temperature sensor 2 (%s) is very high!", TEMP_SENSOR_LABEL2);
          Log.println(reportStr);
        }
      }
      sprintf(reportStr, "Temperature sensor 2 (%s) = %f degrees Celcius", TEMP_SENSOR_LABEL2, theTempSensor2.temperature);
    }  
    Log.println(reportStr);

    // Log machine state
    switch (machinestate) {
      case SWITCHEDOFF:
          Log.println("Compressor is switched off");
        break;
      case POWERED:
        if (!compressorIsOn) {
          Log.println("Compressor is switched on, motor is off");
        } else {
          Log.println("Compressor is switched on, motor is off");
        }
        break;
      case RUNNING:
        Log.println("Compressor is switched on, motor is running");
        break;
      case REBOOT:
      case WAITINGFORCARD:
      case CHECKINGCARD:
      case TRANSIENTERROR:
      case OUTOFORDER:
      case NOCONN:
      case BOOTING:
        break;
    }
  }
}


#ifdef TEST_TIMING
void testLoopTiming(const char LoopPlace[]) {
  long long timeDiff;
  char tmpStr[50];

  timeDiff = millis() - prevTime;
  averageTime[placeCount] = averageTime[placeCount] + timeDiff;

  if (waitcount >= 999) {
    if ((averageTime[placeCount] > TEST_TIME_LIMIT) || (TEST_TIME_LIMIT <= 0)) {
      sprintf(tmpStr, "%s: %lld ms", LoopPlace, averageTime[placeCount]);
      Serial.println(tmpStr);
    }
    averageTime[placeCount] = 0;
  }
  placeCount++;
  prevTime = millis();
}
#endif

void loop() {
#ifdef TEST_TIMING
  prevTime = millis();
  placeCount = 0;
#endif

  node.loop();
#ifdef TEST_TIMING
  testLoopTiming("na node.loop");
#endif

  theTempSensor1.loop();
  theTempSensor2.loop();
#ifdef TEST_TIMING
  testLoopTiming("na theTempSensor1 en 2.loop");
#endif

  thePressureSensor.loop();
#ifdef TEST_TIMING
  testLoopTiming("na thePressureSensor.loop");
#endif

  if (!showLedDisable) {
    theOledDisplay.loop(oilLevelIsTooLow, ErrorOilLevelIsTooLow, 
                        theTempSensor1.temperature, theTempSensor1.tempIsHigh, theTempSensor1.ErrorTempIsTooHigh, 
                        theTempSensor2.temperature, theTempSensor2.tempIsHigh, theTempSensor2.ErrorTempIsTooHigh, ErrorPressureIsTooHigh,
                        pressure, machinestate, 
                        powered_total, powered_last,
                        running_total, running_last);
  }
#ifdef TEST_TIMING
  testLoopTiming("na theOledDisplay.loop");
#endif

  compressorLoop();
#ifdef TEST_TIMING
  testLoopTiming("na compressorLoop");
#endif


  buttons_optocoupler_loop();
#ifdef TEST_TIMING
  testLoopTiming("na buttons_optocoupler.loop");
#endif


  theOilLevelSensor.loop();
#ifdef TEST_TIMING
  testLoopTiming("na theOilLevelSensor.loop");
#endif

  if (laststate != machinestate) {
    Log.print("Changed from state ");
    Log.print(state[laststate].label);
    Log.print(" to state ");
    Log.println(state[machinestate].label);

    if (machinestate >= POWERED && laststate < POWERED) {
      powered_last = millis();
    } else if (laststate >= POWERED && machinestate < POWERED) {
      powered_total += (millis() - powered_last) / 1000;
      powered = (float)powered_total / 3600.0;
    };
    if (machinestate == RUNNING && laststate < RUNNING) {
      running_last = millis();
    } else if (laststate == RUNNING && machinestate < RUNNING) {
      running_total += (millis() - running_last) / 1000;
      running = (float)running_total / 3600.0;
    };
    laststate = machinestate;
    laststatechange = millis();
  }

  if (state[machinestate].maxTimeInMilliSeconds != NEVER &&
      ((millis() - laststatechange) > state[machinestate].maxTimeInMilliSeconds)) {
    laststate = machinestate;
    machinestate = state[machinestate].failStateOnTimeout;
//    Debug.print("Time-out; transition from ");
//    Debug.print(state[laststate].label);
//    Debug.print(" to ");
//    Debug.println(state[machinestate].label);
    return;
  };
  
  switch (machinestate) {
    case REBOOT:
      saveDurationCounters();
      node.delayedReboot();
      break;

    case SWITCHEDOFF:
      // Compressor switched off completely.
      if (compressorIsOn) {
        CompressorMustBeSwitchedOff = false;
        digitalWrite(RELAY_GPIO, 0);
        // digitalWrite(LED1, 0);
        // digitalWrite(LED2, 0);
        ledcWrite(PWM_LED_CHANNEL1, 0);
        ledcWrite(PWM_LED_CHANNEL2, 0);
        compressorIsOn = false;
        Log.println("Compressor switched off");
      }
      break;

    case POWERED:
      // Compressor switched on, but motor is off
      if (!compressorIsOn) {
        digitalWrite(RELAY_GPIO, 1);
        // digitalWrite(LED1, 1);
        // digitalWrite(LED2, 0);
        ledcWrite(PWM_LED_CHANNEL1, LED1_DIM_VALUE);
        ledcWrite(PWM_LED_CHANNEL2, 0);
        compressorIsOn = true;
        Log.println("Compressor switched on, motor is off");
      }
      break;
    case RUNNING:
      // Compressor switched on and motor is running
      break;

    case WAITINGFORCARD:
    case CHECKINGCARD:
      // should not come here, so bail out
      machinestate = REBOOT;
      break;
    case TRANSIENTERROR:
    case OUTOFORDER:
    case NOCONN:
    case BOOTING:
      break;
  };

#ifdef TEST_TIMING
  testLoopTiming("na state machine (einde loop)");

  waitcount++;
  if (waitcount >= 1000) {
    waitcount = 0;
  }
#endif
}


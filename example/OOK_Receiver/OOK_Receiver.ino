/*
 Basic rtl_433_ESP example for OOK/ASK Devices

 CC1101 tuning build flags:
   -DCC1101_OOK_TUNING
   -DRF_MODULE_FREQUENCY=345.0

 The suite runs frequency, bandwidth, AGCCTRL2, and AGCCTRL0 phases in order,
 automatically carries each winner forward, then validates the final setting.
 Optional starting values:
   -DCC1101_TUNING_BASE_BANDWIDTH=406.0
   -DCC1101_TUNING_BASE_AGCCTRL2=0x03
   -DCC1101_TUNING_BASE_AGCCTRL0=0x91

 Each setting runs for 300 seconds by default. Override with
 CC1101_TUNING_WINDOW_SECONDS and CC1101_TUNING_SIGNAL_INTERVAL_SECONDS.
*/

#include <ArduinoJson.h>
#include <ArduinoLog.h>
#include <rtl_433_ESP.h>

#ifndef RF_MODULE_FREQUENCY
#  define RF_MODULE_FREQUENCY 433.92
#endif

#define JSON_MSG_BUFFER            512
#define RAW_CALLBACK_SAMPLE_PULSES 8

char messageBuffer[JSON_MSG_BUFFER];

rtl_433_ESP rf; // use -1 to disable transmitter

volatile int count = 0;

#if defined(CC1101_OOK_TUNING)
#  if !defined(RF_CC1101)
#    error "CC1101_OOK_TUNING requires RF_CC1101"
#  endif
#  ifndef CC1101_TUNING_WINDOW_SECONDS
#    define CC1101_TUNING_WINDOW_SECONDS 300UL
#  endif
#  ifndef CC1101_TUNING_SIGNAL_INTERVAL_SECONDS
#    define CC1101_TUNING_SIGNAL_INTERVAL_SECONDS 30UL
#  endif
#  ifndef CC1101_TUNING_BASE_BANDWIDTH
#    define CC1101_TUNING_BASE_BANDWIDTH 406.0f
#  endif
#  ifndef CC1101_TUNING_BASE_AGCCTRL2
#    define CC1101_TUNING_BASE_AGCCTRL2 0x03
#  endif
#  ifndef CC1101_TUNING_BASE_AGCCTRL0
#    define CC1101_TUNING_BASE_AGCCTRL0 0x91
#  endif

struct CC1101TuningSetting {
  float frequencyMHz;
  float bandwidthKHz;
  uint8_t agcctrl2;
  uint8_t agcctrl0;
};

volatile unsigned int rawCount = 0;
volatile unsigned long rawPulseTotal = 0;
volatile long rawRssiTotal = 0;
volatile int rawRssiMin = 127;
volatile int rawRssiMax = -256;

enum CC1101TuningPhase : uint8_t {
  TUNING_FREQUENCY = 1,
  TUNING_BANDWIDTH,
  TUNING_AGCCTRL2,
  TUNING_AGCCTRL0,
  TUNING_FINAL_VALIDATION
};

const float frequencyValues[] = {344.50f, 344.60f, 344.70f, 344.80f,
                                 344.90f, 345.00f, 345.10f, 345.20f,
                                 345.30f, 345.40f, 345.50f};
const float bandwidthValues[] = {812.0f, 650.0f, 406.0f, 325.0f, 270.0f,
                                 203.0f, 162.0f, 116.0f, 81.0f, 58.0f};
const uint8_t agcctrl2Values[] = {0x03, 0x07, 0x43, 0x83, 0xC7};
const uint8_t agcctrl0Values[] = {0x90, 0x91, 0x92, 0x93};

CC1101TuningPhase tuningPhase = TUNING_FREQUENCY;
CC1101TuningSetting selectedSetting = {
    RF_MODULE_FREQUENCY, CC1101_TUNING_BASE_BANDWIDTH,
    CC1101_TUNING_BASE_AGCCTRL2, CC1101_TUNING_BASE_AGCCTRL0};
size_t tuningSettingIndex = 0;
unsigned long tuningWindowStartedMs = 0;
int bestDecoded = -1;
unsigned int bestRawDistance = 0xFFFF;
int bestRssi = -1000;
CC1101TuningSetting bestSetting;
#endif

void rtl_433_Callback(char* message) {
  JsonDocument jsonDocument;
  deserializeJson(jsonDocument, message);
  jsonDocument["time_ms"] = millis();
  logJson(jsonDocument);
  count++;
}

void rtl_433_RawCallback(const int* pulse_us, const int* gap_us,
                         unsigned int num_pulses, unsigned long duration_us,
                         int rssi) {
#if defined(CC1101_OOK_TUNING)
  (void)pulse_us;
  (void)gap_us;
  (void)duration_us;
  rawCount++;
  rawPulseTotal += num_pulses;
  rawRssiTotal += rssi;
  if (rssi < rawRssiMin) rawRssiMin = rssi;
  if (rssi > rawRssiMax) rawRssiMax = rssi;
#else
  Log.notice(F("Raw signal: time_ms=%u duration=%uus pulses=%u rssi=%d" CR),
             millis(), duration_us, num_pulses, rssi);
  unsigned int sampleCount = num_pulses;
  if (sampleCount > RAW_CALLBACK_SAMPLE_PULSES) {
    sampleCount = RAW_CALLBACK_SAMPLE_PULSES;
  }
  for (unsigned int i = 0; i < sampleCount; i++) {
    Log.notice(F("  pulse[%u]=+%dus -%dus" CR), i, pulse_us[i], gap_us[i]);
  }
#endif
}

void logJson(JsonDocument jsondata) {
#if defined(ESP8266) || defined(ESP32) || defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
  char JSONmessageBuffer[measureJson(jsondata) + 1];
  serializeJson(jsondata, JSONmessageBuffer, measureJson(jsondata) + 1);
#else
  char JSONmessageBuffer[JSON_MSG_BUFFER];
  serializeJson(jsondata, JSONmessageBuffer, JSON_MSG_BUFFER);
#endif
#if defined(CC1101_OOK_TUNING)
  Log.setShowLevel(false);
  Log.notice(F("."));
  Log.setShowLevel(true);
#else
  Log.notice(F("Received message : %s" CR), JSONmessageBuffer);
#endif
}

#if defined(CC1101_OOK_TUNING)
const char* tuningPhaseName() {
  switch (tuningPhase) {
    case TUNING_FREQUENCY: return "frequency";
    case TUNING_BANDWIDTH: return "bandwidth";
    case TUNING_AGCCTRL2: return "agcctrl2";
    case TUNING_AGCCTRL0: return "agcctrl0";
    default: return "final_validation";
  }
}

size_t tuningSettingCount() {
  switch (tuningPhase) {
    case TUNING_FREQUENCY: return sizeof(frequencyValues) / sizeof(frequencyValues[0]);
    case TUNING_BANDWIDTH: return sizeof(bandwidthValues) / sizeof(bandwidthValues[0]);
    case TUNING_AGCCTRL2: return sizeof(agcctrl2Values) / sizeof(agcctrl2Values[0]);
    case TUNING_AGCCTRL0: return sizeof(agcctrl0Values) / sizeof(agcctrl0Values[0]);
    default: return 1;
  }
}

CC1101TuningSetting tuningSetting(size_t index) {
  CC1101TuningSetting setting = selectedSetting;
  switch (tuningPhase) {
    case TUNING_FREQUENCY: setting.frequencyMHz = frequencyValues[index]; break;
    case TUNING_BANDWIDTH: setting.bandwidthKHz = bandwidthValues[index]; break;
    case TUNING_AGCCTRL2: setting.agcctrl2 = agcctrl2Values[index]; break;
    case TUNING_AGCCTRL0: setting.agcctrl0 = agcctrl0Values[index]; break;
    default: break;
  }
  return setting;
}

void resetTuningStatistics() {
  count = 0;
  rawCount = 0;
  rawPulseTotal = 0;
  rawRssiTotal = 0;
  rawRssiMin = 127;
  rawRssiMax = -256;
}

void applyTuningSetting(size_t index) {
  CC1101TuningSetting setting = tuningSetting(index);
  rf.disableReceiver();

  int16_t frequencyState = rf.setFrequency(setting.frequencyMHz);
  int16_t bandwidthState = rf.setRxBandwidth(setting.bandwidthKHz);
  int16_t agc2State = rf.setCC1101Register(
      RADIOLIB_CC1101_REG_AGCCTRL2, setting.agcctrl2);
  int16_t agc0State = rf.setCC1101Register(
      RADIOLIB_CC1101_REG_AGCCTRL0, setting.agcctrl0);
  int16_t receiveState = rf.receiveDirect();
  rf.enableReceiver();

  resetTuningStatistics();
  tuningWindowStartedMs = millis();

  uint8_t mdmcfg4 = rf.getCC1101Register(RADIOLIB_CC1101_REG_MDMCFG4);
  uint8_t agcctrl2 = rf.getCC1101Register(RADIOLIB_CC1101_REG_AGCCTRL2);
  uint8_t agcctrl1 = rf.getCC1101Register(RADIOLIB_CC1101_REG_AGCCTRL1);
  uint8_t agcctrl0 = rf.getCC1101Register(RADIOLIB_CC1101_REG_AGCCTRL0);
  Log.notice(
      F(CR "TUNING_START phase=%s setting=%u/%u frequency_mhz=%F bandwidth_khz=%F "
           "agcctrl2=0x%02X agcctrl1=0x%02X agcctrl0=0x%02X mdmcfg4=0x%02X "
           "window_s=%u expected=%u states=%d,%d,%d,%d,%d" CR),
      tuningPhaseName(), (unsigned int)(index + 1),
      (unsigned int)tuningSettingCount(), (double)setting.frequencyMHz,
      (double)setting.bandwidthKHz, agcctrl2, agcctrl1, agcctrl0, mdmcfg4,
      (unsigned int)CC1101_TUNING_WINDOW_SECONDS,
      (unsigned int)(CC1101_TUNING_WINDOW_SECONDS /
                     CC1101_TUNING_SIGNAL_INTERVAL_SECONDS),
      frequencyState, bandwidthState, agc2State, agc0State, receiveState);
}

void finishTuningSetting() {
  CC1101TuningSetting setting = tuningSetting(tuningSettingIndex);
  unsigned long elapsedMs = millis() - tuningWindowStartedMs;
  unsigned int capturedRawCount = rawCount;
  int capturedDecodedCount = count;
  unsigned long capturedPulseTotal = rawPulseTotal;
  long capturedRssiTotal = rawRssiTotal;
  int capturedRssiMin = rawRssiMin;
  int capturedRssiMax = rawRssiMax;

  long meanRssi = capturedRawCount ? capturedRssiTotal / (long)capturedRawCount : 0;
  unsigned long meanPulses =
      capturedRawCount ? capturedPulseTotal / capturedRawCount : 0;
  Log.notice(
      F("TUNING_RESULT phase=%s setting=%u/%u frequency_mhz=%F bandwidth_khz=%F "
        "agcctrl2=0x%02X agcctrl0=0x%02X elapsed_s=%u expected=%u raw=%u "
        "decoded=%d rssi_mean=%d rssi_min=%d rssi_max=%d pulses_mean=%u" CR),
      tuningPhaseName(), (unsigned int)(tuningSettingIndex + 1),
      (unsigned int)tuningSettingCount(), (double)setting.frequencyMHz,
      (double)setting.bandwidthKHz, setting.agcctrl2, setting.agcctrl0,
      (unsigned int)(elapsedMs / 1000UL),
      (unsigned int)(CC1101_TUNING_WINDOW_SECONDS /
                     CC1101_TUNING_SIGNAL_INTERVAL_SECONDS),
      capturedRawCount, capturedDecodedCount, (int)meanRssi,
      capturedRawCount ? capturedRssiMin : 0,
      capturedRawCount ? capturedRssiMax : 0, (unsigned int)meanPulses);

  unsigned int expected = CC1101_TUNING_WINDOW_SECONDS /
                          CC1101_TUNING_SIGNAL_INTERVAL_SECONDS;
  unsigned int rawDistance = capturedRawCount > expected
                                 ? capturedRawCount - expected
                                 : expected - capturedRawCount;
  if (capturedDecodedCount > bestDecoded ||
      (capturedDecodedCount == bestDecoded && rawDistance < bestRawDistance) ||
      (capturedDecodedCount == bestDecoded && rawDistance == bestRawDistance &&
       meanRssi > bestRssi)) {
    bestDecoded = capturedDecodedCount;
    bestRawDistance = rawDistance;
    bestRssi = meanRssi;
    bestSetting = setting;
  }
}

void completeTuningPhase() {
  selectedSetting = bestSetting;
  Log.notice(
      F("TUNING_WINNER phase=%s frequency_mhz=%F bandwidth_khz=%F "
        "agcctrl2=0x%02X agcctrl0=0x%02X decoded=%d raw_distance=%u rssi=%d" CR),
      tuningPhaseName(), (double)selectedSetting.frequencyMHz,
      (double)selectedSetting.bandwidthKHz, selectedSetting.agcctrl2,
      selectedSetting.agcctrl0, bestDecoded, bestRawDistance, bestRssi);

  if (tuningPhase < TUNING_FINAL_VALIDATION) {
    tuningPhase = (CC1101TuningPhase)(tuningPhase + 1);
  }
  tuningSettingIndex = 0;
  bestDecoded = -1;
  bestRawDistance = 0xFFFF;
  bestRssi = -1000;

  if (tuningPhase == TUNING_FINAL_VALIDATION) {
    Log.notice(
        F("TUNING_SUITE_COMPLETE frequency_mhz=%F bandwidth_khz=%F "
          "agcctrl2=0x%02X agcctrl0=0x%02X; starting repeated validation" CR),
        (double)selectedSetting.frequencyMHz,
        (double)selectedSetting.bandwidthKHz, selectedSetting.agcctrl2,
        selectedSetting.agcctrl0);
  }
}
#endif

void setup() {
  Serial.begin(921600);
  delay(1000);
#ifndef LOG_LEVEL
  LOG_LEVEL_SILENT
#endif
  Log.begin(LOG_LEVEL, &Serial);
  Log.notice(F(" " CR));
  Log.notice(F("****** setup ******" CR));
  rf.initReceiver(RF_MODULE_RECEIVER_GPIO, RF_MODULE_FREQUENCY);
  Log.notice(F("RF_MODULE_FREQUENCY %F" CR), (double)RF_MODULE_FREQUENCY);
  rf.setCallback(rtl_433_Callback, messageBuffer, JSON_MSG_BUFFER);
#if defined(CC1101_OOK_TUNING)
  rf.setRawPulsesCallback(rtl_433_RawCallback);
#endif
  rf.enableReceiver();
  Log.notice(F("****** setup complete ******" CR));
  rf.getModuleStatus();
#if defined(CC1101_OOK_TUNING)
  Log.notice(F("CC1101 OOK hands-off tuning enabled; %u total test settings" CR),
             (unsigned int)(sizeof(frequencyValues) / sizeof(frequencyValues[0]) +
                            sizeof(bandwidthValues) / sizeof(bandwidthValues[0]) +
                            sizeof(agcctrl2Values) / sizeof(agcctrl2Values[0]) +
                            sizeof(agcctrl0Values) / sizeof(agcctrl0Values[0])));
  applyTuningSetting(tuningSettingIndex);
#endif
}

void loop() {
  rf.loop();
#if defined(CC1101_OOK_TUNING)
  if (millis() - tuningWindowStartedMs >=
      CC1101_TUNING_WINDOW_SECONDS * 1000UL) {
    finishTuningSetting();
    tuningSettingIndex++;
    if (tuningSettingIndex >= tuningSettingCount()) {
      completeTuningPhase();
    }
    applyTuningSetting(tuningSettingIndex);
  }
#endif
}

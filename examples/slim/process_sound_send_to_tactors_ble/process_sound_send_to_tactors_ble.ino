// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// This is an example Arduino app for slim board that demonstrates audio to
// tactile pipeline with a BLE interface. App gets the data from the analog or
// PDM microphone, sends it to the tactile processor. The processed data is
// converted to PWM and sent to the tactors.
//
// Pwm to output mapping, when using UpdatePwmModuleChannel():
// (pwm module, pwm channel) - output channel on the flex connector
// (0, 0) - 1
// (0, 1) - 2
// (0, 2) - 3
// (0, 3) - 4
// (1, 0) - 5
// (1, 1) - 6
// (1, 2) - 7
// (1, 3) - 8
// (2, 0) - 9
// (2, 1) - 10
// (2, 2) - 11
// (2, 3) - 12
//
// Physically on the bracelet the hardware channel order is as follows:
// (6)---(5)---(8)---(12)---(9)---(10)---(7)---(11) << Physical mapping 1-index
// (5)---(4)---(7)---(11)---(8)---( 9)---(6)---(10) << Physical mapping 0-index
//
// With the default ChannelMap, tactors correspond to logical channels as:
// bb----aa----uw----fric---iy-----eh----sh f---ih  << Tactile processor naming
// (0)---(1)---(2)---( 9)---(4)---( 5)---(8)---(3)  << Tactile processor number
//
// bb is baseband.
// The rightmost tactor is in the electronics box.

#include <algorithm>

#include "analog_external_mic.h"
#include "battery_monitor.h"
#include "ble_com.h"
#include "dsp/datestamp.h"
#include "dsp/serialize.h"
#include "flash_settings.h"
#include "look_up.h"
#include "pdm.h"
#include "post_processor_cpp.h"
#include "pwm_sleeve.h"
#include "tactile/envelope_tracker.h"
#include "tactile/tactile_pattern.h"
#include "tactile_processor_cpp.h"
#include "temperature_monitor.h"

using namespace audio_tactile;

// Pick which microphone to use.
#define USING_ANALOG_MIC 1
#define USING_PDM_MIC 0

// Compile time constants.
constexpr int kTactileDecimationFactor = 8;
constexpr int kSaadcSampleRateHz = 15625;
constexpr int kCarlBlockSize = 64;
constexpr int kPwmSamplesAllChannels = kNumPwmValues * kNumTotalPwm;
constexpr int kTactileFramesPerCarlBlock =
    kCarlBlockSize / kTactileDecimationFactor;

static uint8_t g_which_pwm_module_triggered;

static bool g_new_mic_data = false;
static bool g_tactor_processor_on = true;

static int16_t g_mic_data[kAdcDataSize];
// Buffer of input audio as floats in [-1, 1].
static float g_audio_input[kAdcDataSize];

TactileProcessorWrapper g_tactile_processor;
PostProcessorWrapper g_post_processor;

// Device name, tuning, and channel gain and assignment.
Settings g_settings;

bool g_low_battery = false;
bool g_vibration_warning = false;

float g_latest_battery_v = 0.0f;
bool g_measure_battery = false;
float g_latest_temperature_c = 0.0f;
bool g_measure_temperature = false;

// Whether BLE is currently connected.
bool g_ble_connected = false;
// Input audio envelope tracker.
EnvelopeTracker g_envelope_tracker;

// Tactile pattern synthesizer.
TactilePattern g_tactile_pattern;
// Buffer for holding the tactile pattern string to play.
char g_tactile_pattern_buffer[kMaxTactilePatternLength + 1];
// True when a tactile pattern is active.
bool g_tactile_pattern_active = false;

// Pointer to the tactile output buffer of g_tactile_processor.
static float* g_tactile_output;

SoftwareTimer g_occasional_tasks_timer;
int g_measure_sensors_counter = 0;
int g_write_settings_countdown = -1;

void AdcNewData();
void FlashLeds();
void OnPwmSequenceEnd();
void OnBleEvent();
void OnPdmNewData();
void OccasionalTasks(TimerHandle_t);

void setup() {
  // Set the indicator led pin to output.
  nrf_gpio_cfg_output(kLedPinBlue);
  nrf_gpio_cfg_output(kLedPinGreen);

#if USING_ANALOG_MIC
  // Initialize analog microphone.
  ExternalAnalogMic.OnAdcDataReady(AdcNewData);
  ExternalAnalogMic.Initialize();
  // Set the TX to zero, to ground it for the microphone 3.5 mm jack
  nrf_gpio_cfg_output(26);
  nrf_gpio_cfg_output(4);
  nrf_gpio_pin_write(26, 0);
  nrf_gpio_pin_write(4, 0);
#endif

#if USING_PDM_MIC
  // Initialize PDM microphone.
  OnBoardMic.Initialize(4, 26);  // Using audio jack
  OnBoardMic.OnPdmDataReady(OnPdmNewData);
  OnBoardMic.Enable();
#endif

  // Initialize battery monitor.
  PuckBatteryMonitor.InitializeLowVoltageInterrupt();
  PuckBatteryMonitor.OnLowBatteryEventListener(LowBatteryWarning);

  FlashLeds();

  // Initialize tactile processor.
  g_tactile_processor.Init(kSaadcSampleRateHz, kCarlBlockSize,
                           kTactileDecimationFactor);
  constexpr float kDefaultGain = 10.0f;
  g_post_processor.Init(g_tactile_processor.GetOutputSampleRate(),
                        g_tactile_processor.GetOutputBlockSize(),
                        g_tactile_processor.GetOutputNumberTactileChannels(),
                        kDefaultGain);

  EnvelopeTrackerInit(&g_envelope_tracker, kSaadcSampleRateHz);

  TactilePatternInit(&g_tactile_pattern,
                     g_tactile_processor.GetOutputSampleRate());
  TactilePatternStart(&g_tactile_pattern, kTactilePatternConfirm);
  g_tactile_pattern_active = true;

  Serial.println("Firmware built " __DATE__);

  // Initialize flash and read settings file.
  FlashSettings.Initialize();
  if (!FlashSettings.have_file_system()) {
    // Failed to find FAT file system on the external flash. This might happen
    // if the flash hasn't been formatted yet with "SdFat_format".
    Serial.println("Warning: No file system found on external flash. Please "
                   "run the SdFat_format example in Adafruit_SPIFlash.");
  } else if (!FlashSettings.ReadSettingsFile(&g_settings)) {
    // Found the file system but not the settings file found. This might be the
    // first time starting up after formatting the flash. Use default settings.
    Serial.println("Flash file system found, but did not find "
                   kFlashSettingsFile "; using default settings.");
  }

  // Use a default name if device_name hasn't been set or is empty.
  constexpr const char* kDefaultDeviceName = "Slim";
  const char* device_name = (*g_settings.device_name)
      ? g_settings.device_name : kDefaultDeviceName;
  BleCom.Init(device_name, OnBleEvent);

  // Initialize PWM.
  // TODO: When starting the device, the tactor buzz arbitrarily for
  // a couple seconds. Figure out how to initialize them more nicely.
  SleeveTactors.OnSequenceEnd(OnPwmSequenceEnd);
  SleeveTactors.Initialize();
  // Warning: issue only in Arduino. When using StartPlayback() it crashes.
  // Looks like NRF_PWM0 module is automatically triggered, and triggering it
  // again here crashes ISR. Temporary fix is to only use nrf_pwm_task_trigger
  // for NRF_PWM1 and NRF_PWM2. To fix might need a nRF52 driver update.
  nrf_pwm_task_trigger(NRF_PWM1, NRF_PWM_TASK_SEQSTART0);
  nrf_pwm_task_trigger(NRF_PWM2, NRF_PWM_TASK_SEQSTART0);

  // Set the timer to sample sensors on multiples of 2.5 seconds
  g_occasional_tasks_timer.begin(2500, OccasionalTasks);
  g_occasional_tasks_timer.start();

  FlashLeds();
}

void HandleMessage(const Message& message) {
  // Delay in seconds to wait between the last update to settings and writing
  // them to flash. This prevents spamming and wearing out the flash.
  //
  // TODO: This delayed writing behavior is potentially confusing, so
  // consider mitigations. E.g. an adaptive delay that increases when there are
  // frequent changes, or UI changes in the BLE app.
  constexpr int kSettingsWriteDelaySeconds = 15;
  // Convert to number of 2.5 second cycles by multiplying by 2/5 (= 1/2.5).
  constexpr int kSettingsWriteDelayCycles =
    (kSettingsWriteDelaySeconds * 2 + 4) / 5;

  switch (message.type()) {
    case MessageType::kGetOnConnectionBatch:
      // Request to get on-connection batch message.
      Serial.println("Message: GetOnConnectionBatch.");
      BleCom.tx_message().WriteOnConnectionBatch(
          /*firmware_build_date=*/DATESTAMP_UINT32,
          /*battery_v=*/g_latest_battery_v,
          /*temperature_c=*/g_latest_temperature_c,
          /*settings=*/g_settings);
      BleCom.SendTxMessage();
      // Play "connect" pattern as UI feedback that connection is established.
      TactilePatternStart(&g_tactile_pattern, kTactilePatternConnect);
      g_tactile_pattern_active = true;
      break;
    case MessageType::kGetTuning:
      // Request message to get the current tuning knobs.
      Serial.println("Message: GetTuning.");
      BleCom.tx_message().WriteTuning(g_settings.tuning);
      BleCom.SendTxMessage();
      // The web interface sends a GetTuning message as soon as it is connected.
      // Play "connect" pattern as UI feedback that connection is established.
      // TactilePatternStart(&g_tactile_pattern, kTactilePatternConnect);
      // g_tactile_pattern_active = true;
      break;
    case MessageType::kTuning:
      // Message specifying new tuning knobs.
      Serial.println("Message: Tuning.");
      if (message.ReadTuning(&g_settings.tuning)) {
        g_tactile_processor.ApplyTuning(g_settings.tuning);
        // Play "confirm" pattern as UI feedback when new settings are applied.
        TactilePatternStart(&g_tactile_pattern, kTactilePatternConfirm);
        g_tactile_pattern_active = true;
        // Settings were updated, so reset countdown to update flash settings.
        g_write_settings_countdown = kSettingsWriteDelayCycles;
      }
      break;
    case MessageType::kTactilePattern:
      // Message to play a tactile pattern.
      Serial.println("Message: TactilePattern.");
      if (message.ReadTactilePattern(g_tactile_pattern_buffer)) {
        TactilePatternStart(&g_tactile_pattern, g_tactile_pattern_buffer);
        g_tactile_pattern_active = true;
      }
      break;
    case MessageType::kGetChannelMap:
      // Request message to get the current channel map.
      Serial.println("Message: GetChannelMap.");
      BleCom.tx_message().WriteChannelMap(g_settings.channel_map);
      BleCom.SendTxMessage();
      break;
    case MessageType::kChannelMap:
      // Message specifying a new channel map.
      Serial.println("Message: ChannelMap.");
      message.ReadChannelMap(&g_settings.channel_map,
                             kTactileProcessorNumTactors,
                             kTactileProcessorNumTactors);
      // Reset countdown to update flash settings.
      g_write_settings_countdown = kSettingsWriteDelayCycles;
      break;
    case MessageType::kChannelGainUpdate:
      Serial.println("Message: ChannelGainUpdate.");
      int test_channels[2];
      if (message.ReadChannelGainUpdate(&g_settings.channel_map, test_channels,
                                        kTactileProcessorNumTactors,
                                        kTactileProcessorNumTactors)) {
        TactilePatternStartCalibrationTones(&g_tactile_pattern,
                                            test_channels[0], test_channels[1]);
        g_tactile_pattern_active = true;
        // Reset countdown to update flash settings.
        g_write_settings_countdown = kSettingsWriteDelayCycles;
      }
      break;
    case MessageType::kCalibrateChannel:
      Serial.println("Message: CalibrateChannel.");
      int calibration_channels[2];
      float calibration_amplitude;
      if (message.ReadCalibrateChannel(&g_settings.channel_map,
                                        calibration_channels,
                                        &calibration_amplitude)) {
        TactilePatternStartCalibrationTonesThresholds(&g_tactile_pattern,
                                            calibration_channels[0],
                                            calibration_channels[1],
                                            calibration_amplitude);
        g_tactile_pattern_active = true;
        // Reset countdown to update flash settings.
        g_write_settings_countdown = kSettingsWriteDelayCycles;
      }
      break;
    case MessageType::kDeviceName:
      Serial.println("Message: DeviceName.");
      if (message.ReadDeviceName(g_settings.device_name)) {
        Serial.print("New device name: \"");
        Serial.print(g_settings.device_name);
        Serial.println("\"");
        // Reset countdown to update flash settings.
        g_write_settings_countdown = kSettingsWriteDelayCycles;
      }
      break;
    case MessageType::kGetDeviceName:
      Serial.println("Message: GetDeviceName.");
      BleCom.tx_message().WriteDeviceName(g_settings.device_name);
      BleCom.SendTxMessage();
      break;
    case MessageType::kPrepareForBluetoothBootloading:
      // Turn off all interrupts, so they don't interfere with bootloading.
      Serial.println("Message: kPrepareForOtaBootloading.");
      SleeveTactors.DisablePwm();
      g_occasional_tasks_timer.stop();
      // Disable onboard or/and external mic.
      ExternalAnalogMic.Disable();
      OnBoardMic.Disable();
      break;
    default:
      Serial.println("Unhandled message.");
      break;
  }
}

void loop() {
  if (g_measure_battery) {
    ExternalAnalogMic.Disable();
    uint16_t battery_read_raw = PuckBatteryMonitor.MeasureBatteryVoltage();
    g_latest_battery_v =
        PuckBatteryMonitor.ConvertBatteryVoltageToFloat(battery_read_raw);
    Serial.println(g_latest_battery_v);

    BleCom.tx_message().WriteBatteryVoltage(g_latest_battery_v);
    BleCom.SendTxMessage();

    ExternalAnalogMic.Initialize();
    g_measure_battery = false;
  }

  if (g_measure_temperature) {
    ExternalAnalogMic.Disable();
    uint16_t temperature_read_raw = SleeveTemperatureMonitor.TakeAdcSample();
    g_latest_temperature_c =
        SleeveTemperatureMonitor.ConvertAdcSampleToTemperature(
            temperature_read_raw);
    Serial.println(g_latest_temperature_c);

    BleCom.tx_message().WriteTemperature(g_latest_temperature_c);
    BleCom.SendTxMessage();

    ExternalAnalogMic.Initialize();
    g_measure_temperature = false;
  }

  if (g_write_settings_countdown == 0) {
    g_write_settings_countdown = -1;
    if(!FlashSettings.have_file_system()) {
      BleCom.tx_message().WriteFlashWriteStatus(kFlashWriteErrorNotFormatted);
      BleCom.SendTxMessage();
    } else {
      if(FlashSettings.WriteSettingsFile(g_settings)) {
        BleCom.tx_message().WriteFlashWriteStatus(kFlashWriteSuccess);
        BleCom.SendTxMessage();
      } else {
        // TODO: Extract and handle flash memory error codes.
        BleCom.tx_message().WriteFlashWriteStatus(kFlashWriteUnkownError);
        BleCom.SendTxMessage();
      }
    }
  }

  if (g_new_mic_data) {
  // Convert ADC values to floats. The raw ADC values can swing from -2048 to
  // 2048, (12 bits) for analog mic and 32,768 for PDM mic (16 bits)
#if USING_ANALOG_MIC
    const float scale = TuningGetInputGain(&g_settings.tuning) / 2048.0f;
#endif

#if USING_PDM_MIC
    const float scale = TuningGetInputGain(&g_settings.tuning) / 32768.0f;
#endif

    for (int i = 0; i < kAdcDataSize; ++i) {
      g_audio_input[i] = scale * g_mic_data[i];
    }

    // Track the envelope of the input audio.
    if (EnvelopeTrackerProcessSamples(&g_envelope_tracker, g_audio_input,
                                      kAdcDataSize) &&
        g_ble_connected) {
      BleCom.tx_message().WriteStatsRecord(g_envelope_tracker);
      BleCom.SendTxMessage();
    }
    // Play synthesized tactile pattern if a pattern is active.
    if (g_tactile_pattern_active) {
      g_tactile_pattern_active = TactilePatternSynthesize(
          &g_tactile_pattern, g_tactile_processor.GetOutputBlockSize(),
          g_tactile_processor.GetOutputNumberTactileChannels(),
          g_tactile_output);
    } else {
      // Process samples.
      g_tactile_output = g_tactile_processor.ProcessSamples(g_audio_input);
    }
    g_post_processor.PostProcessSamples(g_tactile_output);

    g_new_mic_data = false;
    nrf_gpio_pin_toggle(kLedPinGreen);
  }

  if (g_low_battery && !g_vibration_warning) {
    // Play a tactile pattern to warn that the battery is low.
    TactilePatternStart(&g_tactile_pattern, "A66   888   888");
    g_tactile_pattern_active = true;
    g_low_battery = false;
    g_vibration_warning = true;
  }
}

void AdcNewData() {
  ExternalAnalogMic.GetData(g_mic_data);
  g_new_mic_data = true;
}

void OnPdmNewData() {
  OnBoardMic.GetData(g_mic_data);
  g_new_mic_data = true;
}

void OnPwmSequenceEnd() {
  g_which_pwm_module_triggered = SleeveTactors.GetEvent();

  if (g_tactor_processor_on && g_which_pwm_module_triggered == 0) {
    constexpr int kNumChannels = 12;
    // Hardware channel `c` plays logical channel kHwToLogical[c]. Value -1
    // means that nothing is played on that channel.
    constexpr static int kHwToLogical[kNumChannels] = {6, 7, -1, -1, 1, 0,
                                                       8, 2, 4,  5,  3, 9};

    // There are two levels of mapping:
    // 1. Hardware channel `c` plays logical channel kHwToLogical[c],
    //      hw[c] = logical[kHwToLogical[c]].
    // 2. g_settings.channel_map maps tactile channels to logical channels,
    //      logical[c] = channel_map.gains[c] * tactile[channel_map.sources[c]].
    //
    // We compose this into a single step of copying as
    //   hw[c] = channel_map.gains[logical_c]
    //           * tactile[channel_map.sources[logical_c]].
    // with logical_c = kHwToLogical[c].
    for (int c = 0; c < kNumChannels; ++c) {
      const int logical_c = kHwToLogical[c];
      // Skip unused channel. The compiler should be able to unroll this loop
      // and make this iteration a no-op.
      if (logical_c == -1) {
        continue;
      }
      const float gain = g_settings.channel_map.gains[logical_c];
      const float* src =
          g_tactile_output + g_settings.channel_map.sources[logical_c];
      SleeveTactors.UpdateChannelWithGain(c, gain, src,
                                          kTactileProcessorNumTactors);
    }
  }
}

void FlashLeds() {
  for (int i = 0; i < 5; ++i) {
    nrf_gpio_pin_write(kLedPinBlue, 1);
    nrf_gpio_pin_write(kLedPinGreen, 1);
    delay(100);
    nrf_gpio_pin_write(kLedPinBlue, 0);
    nrf_gpio_pin_write(kLedPinGreen, 0);
    delay(100);
  }
}

void OnBleEvent() {
  switch (BleCom.event()) {
    case BleEvent::kConnect:
      Serial.println("BLE: Connected.");
      g_ble_connected = true;
      break;
    case BleEvent::kDisconnect:
      Serial.println("BLE: Disconnected.");
      g_ble_connected = false;

      // On BLE disconnect, write settings immediately if an update is pending.
      if (g_write_settings_countdown > 0) {
        g_write_settings_countdown = 0;
      }
      break;
    case BleEvent::kInvalidMessage:
      Serial.println("BLE: Invalid message.");
      break;
    case BleEvent::kMessageReceived:
      HandleMessage(BleCom.rx_message());
      break;
  }
}

void LowBatteryWarning() {
  if (PuckBatteryMonitor.GetEvent() == 0) {
    nrf_gpio_pin_write(kLedPinBlue, 1);
    g_low_battery = true;
  }
  if (PuckBatteryMonitor.GetEvent() == 1) {
    nrf_gpio_pin_write(kLedPinBlue, 0);
    g_low_battery = false;
  }
}

// TODO: Simplify the handling of these "occasional tasks" with a
// clock, multiple separate timers, or something.
void OccasionalTasks(TimerHandle_t) {
  g_measure_sensors_counter++;
  if (g_measure_sensors_counter % 2 == 1) {
    // Measure battery every 5 seconds, on an odd count.
    g_measure_battery = true;
  } else if (g_measure_sensors_counter % 8 == 0) {
    // Measure temperature every 20 seconds, on an even count.
    g_measure_temperature = true;
    g_measure_sensors_counter = 0;
  }

  if (g_write_settings_countdown > 0) {
    // Decrement countdown for writing settings to flash.
    --g_write_settings_countdown;
  }
}

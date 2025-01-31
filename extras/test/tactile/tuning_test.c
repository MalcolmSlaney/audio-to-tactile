/* Copyright 2020-2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/tactile/tuning.h"

#include <string.h>

#include "src/dsp/logging.h"
#include "src/tactile/enveloper.h"
#include "src/tactile/tactile_processor.h"

/* Checks that `actual` is within 2% of `expected`. */
static int IsClose(float expected, float actual) {
  if (fabs(expected - actual) > 0.02f * fabs(expected)) {
    fprintf(stderr,
            "Error: Expected actual within 2%% of expected, got\n"
            "  actual = %g\n  expected = %g\n",
            actual, expected);
    return 0;
  }
  return 1;
}

static void TestKnobNamesAreUnique(void) {
  puts("TestKnobNamesAreUnique");

  int i;
  int j;
  for (i = 0; i < kNumTuningKnobs; ++i) {
    for (j = i + 1; j < kNumTuningKnobs; ++j) {
      CHECK(strcmp(kTuningKnobInfo[i].name, kTuningKnobInfo[j].name) != 0);
    }
  }
}

/* Test that kTuningKnobInfo data is sensible. */
static void TestTuningKnobInfo(void) {
  puts("TestTuningKnobInfo");

  int knob;
  for (knob = 0; knob < kNumTuningKnobs; ++knob) {
    const TuningKnobInfo* info = &kTuningKnobInfo[knob];

    CHECK(info->min_value < info->max_value);

    const int name_len = strlen(info->name);
    CHECK(name_len >= 3);  /* Knob name has a sensible length. */
    CHECK(name_len <= 30);

    const int description_len = strlen(info->description);
    CHECK(description_len >= 3);  /* Knob description has a sensible length. */
    CHECK(description_len <= 150);

    /* Test that min_value stringified according to `format` is read back. */
    char buffer[64];
    sprintf(buffer, info->format, info->min_value);
    const double parsed_value = strtod(buffer, NULL);
    CHECK(fabs(parsed_value - info->min_value) <= 1e-3 * fabs(info->min_value));
  }
}

static void TestTuningMapControlValue(int knob, int value, float expected) {
  printf("TestTuningMapControlValue(\"%s\", %d)\n",
         kTuningKnobInfo[knob].name, value);
  float mapped_value = TuningMapControlValue(knob, value);
  if (0 == value || value == 255) {
    CHECK(expected == mapped_value); /* Match exactly at the endpoints. */
  } else {
    CHECK(IsClose(expected, mapped_value));
  }
}

static void TestTactileProcessorApplyTuning(void) {
  puts("TestTactileProcessorApplyTuning");
  int trial;
  TactileProcessorParams params;
  TactileProcessorSetDefaultParams(&params);
  TactileProcessor* processor = CHECK_NOTNULL(TactileProcessorMake(&params));

  TuningKnobs tuning_knobs;
  tuning_knobs = kDefaultTuningKnobs; /* First trial tests the default knobs. */

  for (trial = 0; trial < 3; ++trial) {
    float mapped[kNumTuningKnobs];
    int knob;
    for (knob = 0; knob < kNumTuningKnobs; ++knob) {
      mapped[knob] = TuningMapControlValue(knob, tuning_knobs.values[knob]);
    }

    TactileProcessorApplyTuning(processor, &tuning_knobs);

    const Enveloper* enveloper = &processor->enveloper;
    int c;
    for (c = 0; c < kEnveloperNumChannels; ++c) {
      CHECK(IsClose(pow(10.0f, mapped[kKnobOutputGain] / 20.0f),
                    enveloper->channels[c].output_gain));
      CHECK(mapped[kKnobDenoisingBaseband + c] ==
            enveloper->channels[c].denoise_thresh_factor);
      CHECK(EnveloperCrossChannelDiffusionCoeff(
          enveloper, mapped[kKnobCrossChannelTau]) ==
            enveloper->cross_channel_diffusion_coeff);
      CHECK(-mapped[kKnobAgcStrength] == enveloper->agc_exponent);
      CHECK(EnveloperSmootherCoeff(enveloper, mapped[kKnobNoiseTau]) ==
            enveloper->noise_smoother_coeff);
      CHECK(EnveloperSmootherCoeff(enveloper, mapped[kKnobGainTauRelease]) ==
            enveloper->gain_smoother_coeffs[1]);
      CHECK(mapped[kKnobCompressor] == enveloper->compressor_exponent);
    }

    /* Subsequent trials test random knob values. */
    for (knob = 0; knob < kNumTuningKnobs; ++knob) {
      tuning_knobs.values[knob] = rand() % 256;
    }
  }

  TactileProcessorFree(processor);
}

static void TestTuningGetInputGain(void) {
  puts("TestTuningGetInputGain");
  TuningKnobs tuning_knobs = kDefaultTuningKnobs;
  CHECK(IsClose(1.0f, TuningGetInputGain(&tuning_knobs)));

  tuning_knobs.values[kKnobInputGain] = 63;
  CHECK(IsClose(0.098f, TuningGetInputGain(&tuning_knobs)));
}

int main(int argc, char** argv) {
  srand(0);
  TestKnobNamesAreUnique();
  TestTuningKnobInfo();
  TestTactileProcessorApplyTuning();
  TestTuningGetInputGain();

  TestTuningMapControlValue(kKnobInputGain, 0, -40.0f);
  TestTuningMapControlValue(kKnobInputGain, 255, 40.315f);
  TestTuningMapControlValue(kKnobOutputGain, 0, -18.0f);
  TestTuningMapControlValue(kKnobOutputGain, 191, -0.0235f);
  TestTuningMapControlValue(kKnobOutputGain, 255, 6.0f);
  TestTuningMapControlValue(kKnobDenoisingBaseband, 0, 1.0f);
  TestTuningMapControlValue(kKnobDenoisingBaseband, 155, 25.0f);
  TestTuningMapControlValue(kKnobDenoisingBaseband, 255, 200.0f);
  TestTuningMapControlValue(kKnobCrossChannelTau, 0, 0.04f);
  TestTuningMapControlValue(kKnobCrossChannelTau, 51, 0.1f);
  TestTuningMapControlValue(kKnobCrossChannelTau, 255, 4.0f);
  TestTuningMapControlValue(kKnobAgcStrength, 0, 0.1f);
  TestTuningMapControlValue(kKnobAgcStrength, 191, 0.7f);
  TestTuningMapControlValue(kKnobAgcStrength, 255, 0.9f);
  TestTuningMapControlValue(kKnobNoiseTau, 0, 0.04f);
  TestTuningMapControlValue(kKnobNoiseTau, 127, 0.3964f);
  TestTuningMapControlValue(kKnobNoiseTau, 255, 4.0f);
  TestTuningMapControlValue(kKnobGainTauRelease, 0, 0.04f);
  TestTuningMapControlValue(kKnobGainTauRelease, 73, 0.1495f);
  TestTuningMapControlValue(kKnobGainTauRelease, 255, 4.0f);
  TestTuningMapControlValue(kKnobCompressor, 0, 0.1f);
  TestTuningMapControlValue(kKnobCompressor, 96, 0.2506f);
  TestTuningMapControlValue(kKnobCompressor, 255, 0.5f);

  puts("PASS");
  return EXIT_SUCCESS;
}

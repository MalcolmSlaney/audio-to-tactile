// Copyright 2019, 2021 Google LLC
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

#if !defined(__EMSCRIPTEN__)
#error This file must be built with emscripten
#endif

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "dsp/fast_fun.h"
#include "tactile/tactile_processor.h"
#include "extras/tools/sdl/basic_sdl_app.h"
#include "extras/tools/sdl/texture_from_rle_data.h"

// The visualization has nominally 10 tactors even for the bracelet. The unused
// tactors are simply mapped to blank images.
constexpr int kNumTactors = 10;
constexpr int kDecimationFactor = 8;
constexpr int kBlockSize = 64;
constexpr int kOutputBlockSize = kBlockSize / kDecimationFactor;

constexpr int kNumFormFactors = 2;
constexpr int kNumImageAssets = (kNumTactors + 1);
// Defined in run_tactile_processor_assets.c.
extern const uint8_t* kBraceletImageAssetsRle[kNumImageAssets];
extern const uint8_t* kSleeveImageAssetsRle[kNumImageAssets];

struct FormFactorAssets {
  SDL_Texture* images[kNumImageAssets];
  SDL_Rect image_rects[kNumImageAssets];
};

struct {
  BasicSdlApp app;
  FormFactorAssets form_factors[kNumFormFactors];
  int selected_form_factor;
  uint8_t colormap[256 * 3];

  int chunk_size;
  TactileProcessor* tactile_processor;
  float tactile_output[kOutputBlockSize * kNumTactors];
  float volume_decay_coeff;
  float volume[kNumTactors];
} engine;

static void MainTick();
static void GenerateColormap(uint8_t* colormap);

static bool LoadFormFactorAssets(SDL_Renderer* renderer,
    const uint8_t* data[kNumImageAssets], FormFactorAssets* form_factor) {
  for (int i = 0; i < kNumImageAssets; ++i) {
    form_factor->images[i] = CreateTextureFromRleData(
        data[i], renderer, &form_factor->image_rects[i]);
    if (!form_factor->images[i]) { return false; }
  }
  SDL_SetTextureColorMod(form_factor->images[kNumTactors], 0x9d, 0x8c, 0x78);
  return true;
}

// Initializes SDL. This gets called immediately after the emscripten runtime
// has initialized.
extern "C" void EMSCRIPTEN_KEEPALIVE OnLoad() {
  for (int c = 0; c < kNumTactors; ++c) {
    engine.volume[c] = 0.0f;
  }
  engine.selected_form_factor = 0;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "Error: %s\n", SDL_GetError());
    exit(1);
  }

  SDL_EventState(SDL_MOUSEMOTION, SDL_IGNORE);
  // Disable SDL keyboard events. Otherwise, the tab key (to navigate
  // interactive elements) does not work on the web page since SDL captures it.
  SDL_EventState(SDL_TEXTINPUT, SDL_DISABLE);
  SDL_EventState(SDL_KEYDOWN, SDL_DISABLE);
  SDL_EventState(SDL_KEYUP, SDL_DISABLE);

  // Set the event handling loop. This must be set *before* creating the window,
  // otherwise there is an error "Cannot set timing mode for main loop".
  emscripten_set_main_loop(MainTick, 0, 0);

  if (!BasicSdlAppCreate(&engine.app, "", 326, 512, SDL_WINDOW_SHOWN)) {
    exit(1);
  }

  /* Create SDL_Textures from embedded image assets. */
  if (!LoadFormFactorAssets(engine.app.renderer,
                            kBraceletImageAssetsRle,
                            &engine.form_factors[0]) ||
      !LoadFormFactorAssets(engine.app.renderer,
                            kSleeveImageAssetsRle,
                            &engine.form_factors[1])) {
    exit(1);
  }

  GenerateColormap(engine.colormap);
}

// Emscripten will call this function once per frame to do event processing
// (though we ignore all events) and to render the next frame.
static void MainTick() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {}  // Ignore events.

  const FormFactorAssets* assets =
      &engine.form_factors[engine.selected_form_factor];
  SDL_RenderClear(engine.app.renderer);
  // Render background texture.
  SDL_RenderCopy(engine.app.renderer, assets->images[kNumTactors],
                 nullptr, &assets->image_rects[kNumTactors]);

  for (int c = 0; c < kNumTactors; ++c) {
    // Get the RMS value of the cth tactor.
    const float rms = engine.volume[c];
    // Map the RMS in range [rms_min, rms_max] logarithmically to [0, 1].
    constexpr float kRmsMin = 0.003f;
    constexpr float kRmsMax = 0.05f;
    float activation =
        FastLog2(1e-12f + rms / kRmsMin) / FastLog2(kRmsMax / kRmsMin);
    activation = std::min<float>(std::max<float>(activation, 0.0f), 1.0f);

    // Render the cth texture with color according to `activation`.
    const int index = static_cast<int>(std::round(255 * activation));
    const uint8_t* rgb = &engine.colormap[3 * index];
    SDL_SetTextureColorMod(assets->images[c], rgb[0], rgb[1], rgb[2]);
    SDL_RenderCopy(engine.app.renderer, assets->images[c],
                   nullptr, &assets->image_rects[c]);
  }

  SDL_RenderPresent(engine.app.renderer);
}

// Initializes TactileProcessor. This gets called after WebAudio has started.
extern "C" void EMSCRIPTEN_KEEPALIVE TactileInitAudio(
    int sample_rate_hz, int chunk_size) {
  engine.chunk_size = chunk_size;
  TactileProcessorParams params;
  TactileProcessorSetDefaultParams(&params);
  params.decimation_factor = kDecimationFactor;
  params.frontend_params.block_size = kBlockSize;
  params.frontend_params.input_sample_rate_hz = sample_rate_hz;

  for (int c = 0; c < kEnveloperNumChannels; ++c) {
    params.enveloper_params.channel_params[c].output_gain *= 0.5f;
  }

  engine.tactile_processor = TactileProcessorMake(&params);
  if (!engine.tactile_processor) {
    fprintf(stderr, "Error: Failed to create TactileProcessor.\n");
    exit(1);
  }

  constexpr float kVolumeMeterTimeConstantSeconds = 0.05f;
  engine.volume_decay_coeff = std::exp(
      -chunk_size / (kVolumeMeterTimeConstantSeconds * sample_rate_hz));
}

// Processes one chunk of audio data. Called from onaudioprocess.
extern "C" void EMSCRIPTEN_KEEPALIVE TactileProcessAudio(
    intptr_t input_ptr, int chunk_size) {
  float* input = reinterpret_cast<float*>(input_ptr);
  const int num_blocks = chunk_size / kBlockSize;
  float volume_accum[kNumTactors] = {0.0f};

  for (int b = 0; b < num_blocks; ++b) {
    float* tactile = engine.tactile_output;
    TactileProcessorProcessSamples(engine.tactile_processor, input, tactile);

    for (int i = 0; i < kOutputBlockSize; ++i) {
      // For visualization, accumulate energy for each tactile signal.
      for (int c = 0; c < kNumTactors; ++c) {
        volume_accum[c] += tactile[c] * tactile[c];
      }
      tactile += kNumTactors;
    }

    input += kBlockSize;
  }

  for (int c = 0; c < kNumTactors; ++c) {
    // Compute RMS value and update volume meters.
    const float rms = std::sqrt(volume_accum[c]
        / (num_blocks * (kOutputBlockSize * kNumTactors)));
    float updated_volume = engine.volume[c] * engine.volume_decay_coeff;
    if (rms > updated_volume) { updated_volume = rms; }
    engine.volume[c] = updated_volume;
  }
}

// Sets the selected form factor, bracelet (0) or sleeve (1).
extern "C" void EMSCRIPTEN_KEEPALIVE SelectFormFactor(int index) {
  engine.selected_form_factor = index;
}

// Generates a colormap fading from a dark gray to orange to white. `colormap`
// should have space for 256 * 3 elements.
static void GenerateColormap(uint8_t* colormap) {
  const int kColorA[3] = {0x51, 0x43, 0x31};
  const int kColorB[3] = {0xff, 0x6f, 0x00};
  const float kKnot = 0.348f;

  for (int i = 0; i < 256; ++i, colormap += 3) {
    const float x = i / 255.0f;
    if (x <= kKnot) {
      const float w = x / kKnot;
      for (int c = 0; c < 3; ++c) {
        colormap[c] =
            static_cast<int>(kColorA[c] + (kColorB[c] - kColorA[c]) * w);
      }
    } else {
      const float w = (x - kKnot) / (1.0f - kKnot);
      for (int c = 0; c < 3; ++c) {
        colormap[c] = static_cast<int>(kColorB[c] + (255 - kColorB[c]) * w);
      }
    }
  }
}

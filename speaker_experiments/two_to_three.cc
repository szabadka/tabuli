// Copyright 2024 Google LLC
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

#include <algorithm>
#include <complex>
#include <functional>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/check.h"
#include "fftw3.h"
#include "sndfile.hh"

ABSL_FLAG(int, overlap, 128, "how much to overlap the FFTs");
ABSL_FLAG(int, window_size, 4096, "FFT window size");

namespace {

struct FFTWDeleter {
  void operator()(void* p) const { fftwf_free(p); }
};
template <typename T>
using FFTWUniquePtr = std::unique_ptr<T, FFTWDeleter>;

float SquaredNorm(const fftwf_complex c) { return c[0] * c[0] + c[1] * c[1]; }

template <typename In, typename Out>
void Process(const int window_size, const int overlap, In& input_stream,
             Out& output_stream, const std::function<void()>& start_progress,
             const std::function<void(int64_t)>& set_progress) {
  const int skip_size = window_size / overlap;
  const float normalizer = 1.f / (window_size * overlap);

  FFTWUniquePtr<fftwf_complex[]> input_fft(
      fftwf_alloc_complex(2 * (window_size / 2 + 1))),
      center_fft(fftwf_alloc_complex(window_size / 2 + 1));
  FFTWUniquePtr<float[]> input(fftwf_alloc_real(2 * window_size)),
      center(fftwf_alloc_real(window_size));
  std::fill_n(input.get(), 2 * window_size, 0);
  std::vector<float> output(3 * window_size);

  fftwf_plan left_right_fft = fftwf_plan_many_dft_r2c(
      /*rank=*/1, /*n=*/&window_size, /*howmany=*/2, /*in=*/input.get(),
      /*inembed=*/nullptr, /*istride=*/2, /*idist=*/1, /*out=*/input_fft.get(),
      /*onembed=*/nullptr, /*ostride=*/2, /*odist=*/1,
      /*flags=*/FFTW_PATIENT | FFTW_PRESERVE_INPUT);

  fftwf_plan center_ifft = fftwf_plan_dft_c2r_1d(
      /*n0=*/window_size, /*in=*/center_fft.get(), /*out=*/center.get(),
      /*flags=*/FFTW_MEASURE | FFTW_DESTROY_INPUT);

  start_progress();
  int64_t read = 0, written = 0, index = 0;
  for (;;) {
    read += input_stream.readf(input.get() + 2 * (window_size - skip_size),
                               skip_size);
    for (int i = 0; i < skip_size; ++i) {
      output[3 * (window_size - skip_size + i)] =
          input[2 * (window_size - skip_size + i)];
      output[3 * (window_size - skip_size + i) + 1] =
          input[2 * (window_size - skip_size + i) + 1];
      output[3 * (window_size - skip_size + i) + 2] = 0;
    }

    fftwf_execute(left_right_fft);

    for (int i = 0; i < window_size / 2 + 1; ++i) {
      if (SquaredNorm(input_fft[i * 2]) < SquaredNorm(input_fft[i * 2 + 1])) {
        std::copy_n(input_fft[i * 2], 2, center_fft[i]);
      } else {
        std::copy_n(input_fft[i * 2 + 1], 2, center_fft[i]);
      }
    }

    fftwf_execute(center_ifft);

    for (int i = 0; i < window_size; ++i) {
      output[3 * i + 2] += center[i];
    }

    if (index >= window_size - skip_size) {
      for (int i = 0; i < skip_size; ++i) {
        output[3 * i + 2] *= normalizer;
        output[3 * i] -= output[3 * i + 2];
        output[3 * i + 1] -= output[3 * i + 2];
      }
      const int64_t to_write = std::min<int64_t>(skip_size, read - written);
      output_stream.writef(output.data(), to_write);
      written += to_write;
      set_progress(written);
      if (written == read) break;
    }

    std::copy(input.get() + 2 * skip_size, input.get() + 2 * window_size,
              input.get());
    std::fill_n(input.get() + 2 * (window_size - skip_size), 2 * skip_size, 0);
    std::copy(output.begin() + 3 * skip_size, output.end(), output.begin());
    std::fill_n(output.begin() + 3 * (window_size - skip_size), 3 * skip_size,
                0);

    index += skip_size;
  }

  fftwf_destroy_plan(left_right_fft);
  fftwf_destroy_plan(center_ifft);
}

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  const int window_size = absl::GetFlag(FLAGS_window_size);
  const int overlap = absl::GetFlag(FLAGS_overlap);

  QCHECK_EQ(window_size % overlap, 0);

  QCHECK_EQ(argc, 3) << "Usage: " << argv[0] << " <input> <output>";

  SndfileHandle input_file(argv[1]);
  QCHECK(input_file) << input_file.strError();

  QCHECK_EQ(input_file.channels(), 2);

  SndfileHandle output_file(
      argv[2], /*mode=*/SFM_WRITE, /*format=*/SF_FORMAT_WAV | SF_FORMAT_PCM_24,
      /*channels=*/3, /*samplerate=*/input_file.samplerate());

  Process(
      window_size, overlap, input_file, output_file, [] {},
      [](const int64_t written) {});
}

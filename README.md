# DevRev

Speech dereverberation / echo / delay removal plugin (VST3 + AU). Original DSP in `core/`
(online WPE, late-reverb suppression with blind RT60 estimation, echo-delay detection with sparse
linear-prediction taps, cepstral gain smoothing, harmonic/onset repair). No generative synthesis.

- macOS: `./build-mac.sh` (Xcode + CMake), or push and let `.github/workflows/mac.yml` build it.
- Windows: Visual Studio — `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_ROOT=<sdk>`;
  or MinGW from Linux with `mingw-toolchain.cmake` (+ `alignedalloc.mingw-patched.h` copied into the SDK).
- `test/` holds an 8 s reference render; `build-mac.sh` checks that the Mac build reproduces it.

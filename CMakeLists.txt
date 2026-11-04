cmake_minimum_required(VERSION 3.25.0)

if(APPLE)
  # Universal binary (Apple Silicon + Intel) unless ARCHS overrides it
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "")
  set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "" FORCE)
endif()

project(DevRev VERSION 1.0.0 DESCRIPTION "DevRev speech dereverberation" LANGUAGES C CXX)

set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../vst3sdk" CACHE PATH "Path to the Steinberg VST3 SDK")
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)

add_subdirectory(${VST3_SDK_ROOT} ${PROJECT_BINARY_DIR}/vst3sdk)
smtg_enable_vst3_sdk()
if(MINGW AND TARGET validator)
  target_link_options(validator PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

smtg_add_vst3plugin(DevRev
    source/ids.h source/version.h
    source/processor.h source/processor.cpp
    source/controller.h source/controller.cpp
    source/entry.cpp
    core/DevRevCore.h core/DevRevCore.cpp core/RealFFT.h)
target_include_directories(DevRev PRIVATE core)
target_compile_features(DevRev PUBLIC cxx_std_17)
target_link_libraries(DevRev PRIVATE sdk)

# The DSP relies on IEEE semantics (isfinite safety checks); the SDK enables fast-math for Xcode builds.
if(NOT MSVC)
  target_compile_options(DevRev PRIVATE -fno-fast-math)
endif()
if(XCODE)
  set_target_properties(DevRev PROPERTIES XCODE_ATTRIBUTE_GCC_FAST_MATH NO)
endif()

# offline renderer used for the cross-platform DSP check
add_executable(devrev_cli tools/devrev_cli.cpp core/DevRevCore.cpp)
target_include_directories(devrev_cli PRIVATE core)
target_compile_features(devrev_cli PRIVATE cxx_std_17)
if(NOT MSVC)
  target_compile_options(devrev_cli PRIVATE -O2 -fno-fast-math)
endif()
if(XCODE)
  set_target_properties(devrev_cli PROPERTIES XCODE_ATTRIBUTE_GCC_FAST_MATH NO)
endif()

if(APPLE)
  smtg_target_set_bundle(DevRev
    BUNDLE_IDENTIFIER "com.localbuild.devrev.vst3"
    COMPANY_NAME "DevRev")
  if(XCODE AND SMTG_ENABLE_AUV2_BUILDS)
    list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_ROOT}/cmake/modules")
    include(SMTG_AddVST3AuV2)
    smtg_target_add_auv2(DevRev-AU
      BUNDLE_NAME DevRev
      BUNDLE_IDENTIFIER "com.localbuild.devrev.audiounit"
      INFO_PLIST_TEMPLATE ${CMAKE_CURRENT_SOURCE_DIR}/resource/au-info.plist
      VST3_PLUGIN_TARGET DevRev)
  else()
    message(STATUS "DevRev: AU target skipped (needs -G Xcode and Apple's AudioUnitSDK)")
  endif()
endif()

if(MINGW)
  target_compile_options(DevRev PRIVATE -O2 -msse2)
  target_link_options(DevRev PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(SMTG_WIN)
  target_sources(DevRev PRIVATE resource/win32resource.rc)
endif()

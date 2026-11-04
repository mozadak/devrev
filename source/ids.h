#pragma once
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace devrev {
static const Steinberg::FUID kProcessorUID(0x6D3A1C42, 0x9B7E4F15, 0xA2C80E57, 0x3F61D0B9);
static const Steinberg::FUID kControllerUID(0x1E84B2A7, 0x55C94D03, 0x8F2A6B1C, 0xD7409E63);

enum ParamID : Steinberg::Vst::ParamID {
    kDereverb = 0, kRepair, kAmbience, kEcho, kOutput, kBypass,
    kMeterRt60 = 100, kMeterEcho = 101, kMeterReduction = 102
};
// plain ranges
constexpr double kDereverbMax = 150.0, kDereverbDef = 80.0;
constexpr double kRepairDef = 80.0, kAmbienceDef = 10.0;
constexpr double kOutMin = -12.0, kOutMax = 12.0;
constexpr double kRtMax = 3.0, kEchoMax = 1000.0, kRedMin = -40.0;
}

#include "controller.h"
#include "ids.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace devrev {

tresult PLUGIN_API Controller::initialize(FUnknown* context) {
    tresult r = EditController::initialize(context);
    if (r != kResultOk) return r;

    auto* p = new RangeParameter(STR16("DeReverb"), kDereverb, STR16("%"), 0.0, kDereverbMax, kDereverbDef, 0,
                                 ParameterInfo::kCanAutomate);
    p->setPrecision(0); parameters.addParameter(p);

    p = new RangeParameter(STR16("Repair"), kRepair, STR16("%"), 0.0, 100.0, kRepairDef, 0, ParameterInfo::kCanAutomate);
    p->setPrecision(0); parameters.addParameter(p);

    p = new RangeParameter(STR16("Ambience"), kAmbience, STR16("%"), 0.0, 100.0, kAmbienceDef, 0, ParameterInfo::kCanAutomate);
    p->setPrecision(0); parameters.addParameter(p);

    auto* echo = new StringListParameter(STR16("Echo/Delay Removal"), kEcho, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    echo->appendString(STR16("Off")); echo->appendString(STR16("On"));
    echo->setNormalized(1.0); echo->getInfo().defaultNormalizedValue = 1.0;
    parameters.addParameter(echo);

    p = new RangeParameter(STR16("Output"), kOutput, STR16("dB"), kOutMin, kOutMax, 0.0, 0, ParameterInfo::kCanAutomate);
    p->setPrecision(1); parameters.addParameter(p);

    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kBypass);

    p = new RangeParameter(STR16("Room RT60"), kMeterRt60, STR16("s"), 0.0, kRtMax, 0.0, 0, ParameterInfo::kIsReadOnly);
    p->setPrecision(2); parameters.addParameter(p);
    p = new RangeParameter(STR16("Echo Delay"), kMeterEcho, STR16("ms"), 0.0, kEchoMax, 0.0, 0, ParameterInfo::kIsReadOnly);
    p->setPrecision(0); parameters.addParameter(p);
    p = new RangeParameter(STR16("Reduction"), kMeterReduction, STR16("dB"), 0.0, kRedMin, 0.0, 0, ParameterInfo::kIsReadOnly);
    p->setPrecision(1); parameters.addParameter(p);
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* state) {
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    int32 ver = 0; if (!s.readInt32(ver)) return kResultFalse;
    double v[6];
    for (double& x : v) if (!s.readDouble(x)) return kResultFalse;
    const ParamID ids[6] = {kDereverb, kRepair, kAmbience, kEcho, kOutput, kBypass};
    for (int i = 0; i < 6; ++i) setParamNormalized(ids[i], v[i]);
    return kResultOk;
}

} // namespace devrev

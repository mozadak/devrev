#pragma once
#include "public.sdk/source/vst/vsteditcontroller.h"

namespace devrev {

class Controller : public Steinberg::Vst::EditController {
public:
    static Steinberg::FUnknown* createInstance(void*) { return (Steinberg::Vst::IEditController*)new Controller; }
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) SMTG_OVERRIDE;
};

} // namespace devrev

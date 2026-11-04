#include "processor.h"
#include "controller.h"
#include "ids.h"
#include "version.h"
#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)

    DEF_CLASS2(INLINE_UID_FROM_FUID(devrev::kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "DevRev", Vst::kDistributable, "Fx|Restoration", FULL_VERSION_STR, kVstVersionString,
               devrev::Processor::createInstance)

    DEF_CLASS2(INLINE_UID_FROM_FUID(devrev::kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
               "DevRev Controller", 0, "", FULL_VERSION_STR, kVstVersionString,
               devrev::Controller::createInstance)

END_FACTORY

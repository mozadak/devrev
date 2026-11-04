#include "processor.h"
#include "ids.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace devrev {

Processor::Processor() {
    setControllerClass(kControllerUID);
    pDereverb = kDereverbDef / kDereverbMax;
    pRepair = kRepairDef / 100.0;
    pAmbience = kAmbienceDef / 100.0;
    pEcho = 1.0;
    pOutput = (0.0 - kOutMin) / (kOutMax - kOutMin);
    pBypass = 0.0;
}

tresult PLUGIN_API Processor::initialize(FUnknown* context) {
    tresult r = AudioEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Input"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
    return kResultOk;
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                                 SpeakerArrangement* outputs, int32 numOuts) {
    if (numIns == 1 && numOuts == 1 && inputs[0] == outputs[0] &&
        (inputs[0] == SpeakerArr::kMono || inputs[0] == SpeakerArr::kStereo))
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 s) {
    return s == kSample32 ? kResultTrue : kResultFalse;
}

int Processor::numChannels() const {
    if (audioInputs.empty()) return 2;
    auto* bus = FCast<AudioBus>(audioInputs.at(0));
    if (!bus) return 2;
    return std::max(1, std::min(2, SpeakerArr::getChannelCount(bus->getArrangement())));
}

void Processor::prepareCore() {
    const int nch = numChannels();
    core.prepare(sampleRate, nch);
    latency = core.getLatency();
    dryDelay.assign(nch, std::vector<float>(latency, 0.f));
    dryPos = 0;
    scratch.assign(nch, std::vector<float>(8192, 0.f));
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup) {
    sampleRate = setup.sampleRate;
    prepareCore();
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setActive(TBool state) {
    if (state) {
        prepareCore();
        bypassMix = pBypass > 0.5 ? 1.f : 0.f;
    }
    return AudioEffect::setActive(state);
}

uint32 PLUGIN_API Processor::getLatencySamples() { return (uint32)core.getLatency(); }
uint32 PLUGIN_API Processor::getTailSamples() { return (uint32)core.getLatency(); }

static inline float softClip(float x) {
    const float t = 0.891f; // -1 dBFS knee
    float a = std::fabs(x);
    if (a <= t) return x;
    float y = t + (1.f - t) * std::tanh((a - t) / (1.f - t));
    return x < 0 ? -y : y;
}

tresult PLUGIN_API Processor::process(ProcessData& data) {
    if (data.inputParameterChanges) {
        int32 n = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < n; ++i) {
            IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
            if (!q) continue;
            ParamValue v; int32 off;
            if (q->getPoint(q->getPointCount() - 1, off, v) != kResultTrue) continue;
            switch (q->getParameterId()) {
                case kDereverb: pDereverb = v; break;
                case kRepair: pRepair = v; break;
                case kAmbience: pAmbience = v; break;
                case kEcho: pEcho = v; break;
                case kOutput: pOutput = v; break;
                case kBypass: pBypass = v; break;
                default: break;
            }
        }
    }
    Params p;
    p.dereverb = (float)(pDereverb * kDereverbMax / 100.0);
    p.repair = (float)pRepair;
    p.ambience = (float)pAmbience;
    p.echo = pEcho > 0.5;
    core.setParams(p);
    const float targetGain = std::pow(10.f, (float)(kOutMin + pOutput * (kOutMax - kOutMin)) / 20.f);

    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) return kResultOk;
    AudioBusBuffers& in = data.inputs[0];
    AudioBusBuffers& out = data.outputs[0];
    const int nch = std::min<int>({(int)in.numChannels, (int)out.numChannels, (int)dryDelay.size()});
    const int n = data.numSamples;
    if (nch <= 0) return kResultOk;

    // keep a latency-aligned copy of the dry input for bypass crossfades
    int pos = 0;
    while (pos < n) {
        const int blk = std::min(n - pos, (int)scratch[0].size());
        float* ptr[2];
        for (int c = 0; c < nch; ++c) {
            const float* src = in.channelBuffers32[c] + pos;
            float* dst = out.channelBuffers32[c] + pos;
            std::memcpy(scratch[c].data(), src, sizeof(float) * blk);  // dry (in may alias out)
            if (dst != src) std::memcpy(dst, src, sizeof(float) * blk);
            ptr[c] = dst;
        }
        core.process(ptr, blk);
        const float bpTarget = pBypass > 0.5 ? 1.f : 0.f;
        const float step = 1.f / (0.02f * (float)sampleRate);
        int dp = dryPos;
        for (int i = 0; i < blk; ++i) {
            if (bypassMix < bpTarget) bypassMix = std::min(bpTarget, bypassMix + step);
            else if (bypassMix > bpTarget) bypassMix = std::max(bpTarget, bypassMix - step);
            outGain += 0.001f * (targetGain - outGain);
            for (int c = 0; c < nch; ++c) {
                float dry = dryDelay[c][dp];
                dryDelay[c][dp] = scratch[c][i];
                float wet = softClip(ptr[c][i] * outGain);
                ptr[c][i] = bypassMix <= 0.f ? wet : (bypassMix >= 1.f ? dry : wet + bypassMix * (dry - wet));
            }
            if (++dp >= latency) dp = 0;
        }
        dryPos = dp;
        pos += blk;
    }
    // extra output channels (should not happen) are cleared
    for (int c = nch; c < out.numChannels; ++c) std::memset(out.channelBuffers32[c], 0, sizeof(float) * n);
    out.silenceFlags = 0;

    // read-only meters (~10 Hz)
    meterCounter += n;
    if (data.outputParameterChanges && meterCounter >= (int)(sampleRate / 10)) {
        meterCounter = 0;
        const Meters& m = core.meters();
        int32 idx;
        if (auto* q = data.outputParameterChanges->addParameterData(kMeterRt60, idx))
            q->addPoint(0, std::min(1.0, m.rt60 / kRtMax), idx);
        if (auto* q = data.outputParameterChanges->addParameterData(kMeterEcho, idx))
            q->addPoint(0, std::min(1.0, m.echoDelayMs[0] / kEchoMax), idx);
        if (auto* q = data.outputParameterChanges->addParameterData(kMeterReduction, idx))
            q->addPoint(0, std::min(1.0, std::max(0.0, (double)m.reductionDb / kRedMin)), idx);
    }
    return kResultOk;
}

tresult PLUGIN_API Processor::setState(IBStream* state) {
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    int32 ver = 0; if (!s.readInt32(ver)) return kResultFalse;
    double v[6];
    for (double& x : v) if (!s.readDouble(x)) return kResultFalse;
    pDereverb = v[0]; pRepair = v[1]; pAmbience = v[2]; pEcho = v[3]; pOutput = v[4]; pBypass = v[5];
    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state) {
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    s.writeInt32(1);
    for (double x : {pDereverb, pRepair, pAmbience, pEcho, pOutput, pBypass}) s.writeDouble(x);
    return kResultOk;
}

} // namespace devrev

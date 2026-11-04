// DevRev - speech dereverberation / echo removal core.
// Original implementation. Algorithms follow the published literature:
//  - Online WPE (weighted prediction error) dereverberation, Yoshioka & Nakatani 2012/2013
//  - Late-reverb PSD model, Lebart 2001 / Habets 2009
//  - Blind decay-rate (RT60) estimation from free decays, after Ratnam 2003 / Loellmann 2010
//  - Cepstral smoothing of spectral gains, Breithaupt, Gerkmann & Martin 2008
//  - Pitch estimation with window-normalised autocorrelation, Boersma 1993
// No generative synthesis is used anywhere: every output bin is a gain (<= repair cap)
// applied to the linearly-dereverberated signal, so the voice keeps its own phase and timbre.
#pragma once
#include "RealFFT.h"
#include <vector>
#include <complex>
#include <cstdint>

namespace devrev {

using cd = std::complex<double>;

struct Params {
    float dereverb = 0.8f;   // 0..1.5   main amount
    float repair   = 0.8f;   // 0..1     protect/repair voice (harmonics, onsets, timbre)
    float ambience = 0.1f;   // 0..1     natural room left in (raises gain floor)
    bool  echo     = true;   // detect & remove discrete echoes / delays
    bool  linkStereo = true; // shared gains for stereo
};

struct Meters {
    float rt60 = 0.f;         // estimated reverberation time (s)
    float echoDelayMs[2] = {0, 0};
    float echoStrength[2] = {0, 0};
    float voiced = 0.f;       // last frame voicing confidence
    float f0 = 0.f;
    float reductionDb = 0.f;  // broadband reduction last frame
};

class DevRevCore {
public:
    void prepare(double sampleRate, int numChannels);
    void reset();
    void setParams(const Params& p) { prm = p; }
    // In-place processing, any block size. Output is delayed by getLatency() samples.
    void process(float* const* ch, int numSamples);
    int  getLatency() const { return latency; }
    const Meters& meters() const { return met; }

    // tuning knobs (exposed for offline experiments)
    struct Tuning {
        int   wpeDelay = 3;        // prediction delay (frames)
        int   wpeTaps = 14;        // contiguous taps
        double wpeForget = 0.998;
        float wpeMaxHz = 11000.f;
        float lateMu = 1.0f;       // residual-late model weight
        float lateBoundary = 0.f;  // seconds; 0 = auto (beyond WPE window)
        float ddBeta = 0.92f;
        int   mode = 0;
        bool  lateFromY = false;
        bool  gate = true;
        float residFloor = 0.3f;            // 0 full, 1 wpe only, 2 postfilter only
    } tune;

private:
    void processFrame();
    void wpeChannel(int c);
    void analyse();
    void updateRt60();
    void updateEcho();
    void estimatePitch();
    void computeGains();
    void cepstralSmooth();
    void repairGains();
    void timbreEq();
    void tailGate();
    float gateG = 1.f; int tailCount = 0;
    void rebuildWpeTapLayout();

    Params prm; Meters met;
    double fs = 48000.0; int nch = 1;
    int N = 1024, hop = 256, K = 513;  // K = bins
    int latency = 0;
    float binHz = 46.875f;
    RealFFT fft;
    std::vector<float> win, frameBuf;
    std::vector<std::vector<float>> inFifo, outAcc, outFifo;
    int fifoPos = 0;           // samples collected towards next hop
    int64_t frameCount = 0;

    // spectra
    std::vector<std::vector<cf>> Y, Z, S;         // per channel
    std::vector<std::vector<std::vector<cf>>> Yhist; // [c][frame ring][bin]
    int histLen = 0, histPos = 0;

    // WPE state per channel per bin
    struct WpeBin { std::vector<cd> P; std::vector<cd> g; };
    std::vector<std::vector<WpeBin>> wpe;
    std::vector<int> tapLags;   // lags (frames) of the regression vector
    int wpeBins = 0;
    std::vector<float> longPow;  // long-term bin power (mono) for floors
    std::vector<cd> tmpY, tmpPy;

    // analysis (mono / linked)
    std::vector<float> PY, PZ, lamY, lamZ;
    std::vector<std::vector<float>> lamZhist, lamYhist; int lamHistLen = 0;
    std::vector<float> lamI, xiPrev, G, Gcep, cepState, prevPZs;
    std::vector<float> harm; float f0 = 0, voicing = 0;
    std::vector<float> acBuf, winAc; std::vector<cf> specTmp;
    std::vector<float> rnBuf, rBuf, poolBuf, sortBuf; std::vector<const std::vector<cf>*> rowsTmp;
    std::vector<float> logG, cep;

    // RT60 estimator: per band
    static constexpr int NB = 8;
    float bandLo[NB], bandHi[NB];
    float bandDbPrev[NB]; int runLen[NB]; float runStart[NB]; float bandFloor[NB];
public: float rtQuant = 0.6f; float echoThresh = 0.055f; private:
    std::vector<float> runVals[NB];
    std::vector<float> rtEst[NB]; float rt60b[NB];
    float rtGlobal = 0.5f;

    // echo detector
    static constexpr int EB = 16;
    int eMin = 0, eMax = 0;
    std::vector<std::vector<float>> ebHist; int ebPos = 0;
    std::vector<double> eMean, eVar;            // per band
    double eW = 1e-12; int64_t eActive = 0;
    std::vector<std::vector<double>> eCov;      // [lag][band]
    int echoLag[2] = {0, 0}; float echoGain2[2] = {0, 0};
    int echoCand[2] = {0, 0}; int echoCandAge[2] = {0, 0}; int echoMiss[2] = {0, 0};
    std::vector<int> appliedLags;

    // onset / timbre eq
    float frameEnergyHist[32]; int fehPos = 0;
    static constexpr int QB = 12;
    float qLo[QB], qHi[QB]; double accIn[QB], accOut[QB]; float eqGain[QB];
    std::vector<float> eqBin;
    bool onsetAfterPause = false; float broadRise = 0;
};

} // namespace devrev

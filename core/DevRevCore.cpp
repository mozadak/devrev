// DevRev - speech dereverberation / echo removal core. See DevRevCore.h for references.
#include "DevRevCore.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <cstdio>
#include <cstdlib>

namespace devrev {

static inline float sqr(float x) { return x * x; }
static inline float pw(const cf& v) { return v.real() * v.real() + v.imag() * v.imag(); }
static constexpr float kEps = 1e-12f;

void DevRevCore::prepare(double sampleRate, int numChannels) {
    fs = sampleRate; nch = std::max(1, std::min(2, numChannels));
    if (fs < 30000) N = 512; else if (fs <= 50000) N = 1024; else if (fs <= 100000) N = 2048; else N = 4096;
    hop = N / 4; K = N / 2 + 1; binHz = (float)(fs / N);
    latency = N;
    fft.init(N);
    win.resize(N);
    for (int i = 0; i < N; ++i) win[i] = (float)std::sqrt(0.5 - 0.5 * std::cos(2.0 * M_PI * i / N));
    frameBuf.assign(N, 0.f);
    // window autocorrelation for pitch normalisation (frame = x * win)
    {
        std::vector<float> w2(N, 0.f); std::vector<cf> W(K);
        for (int i = 0; i < N; ++i) w2[i] = win[i];
        // zero-padded autocorr would need 2N; window is smooth so circular autocorr is adequate for tau < N/2
        fft.forward(w2.data(), W.data());
        for (auto& v : W) v = cf(pw(v), 0.f);
        winAc.assign(N, 0.f);
        fft.inverse(W.data(), winAc.data());
    }
    acBuf.assign(N, 0.f); specTmp.assign(K, cf()); rnBuf.assign(N + 2, 0.f);
    rowsTmp.reserve(64); poolBuf.reserve(4 * 48 + 8); sortBuf.reserve(64);
    logG.assign(N, 0.f); cep.assign(N, 0.f);

    // bands for RT60
    for (int b = 0; b < NB; ++b) {
        float c = 125.f * std::pow(2.f, (float)b);
        bandLo[b] = c / 1.41421356f; bandHi[b] = std::min(c * 1.41421356f, (float)fs * 0.5f);
    }
    // timbre eq bands
    float lo = 100.f, hi = std::min(16000.f, (float)fs * 0.45f);
    for (int b = 0; b < QB; ++b) {
        qLo[b] = lo * std::pow(hi / lo, (float)b / QB);
        qHi[b] = lo * std::pow(hi / lo, (float)(b + 1) / QB);
    }
    // echo lag range (frames)
    eMin = tune.wpeDelay + tune.wpeTaps + 1;
    eMax = (int)std::round(1.0 * fs / hop);
    reset();
}

void DevRevCore::reset() {
    inFifo.assign(nch, std::vector<float>(N, 0.f));
    outAcc.assign(nch, std::vector<float>(N, 0.f));
    outFifo.assign(nch, std::vector<float>(hop, 0.f));
    fifoPos = 0; frameCount = 0;
    Y.assign(nch, std::vector<cf>(K)); Z = Y; S = Y;
    histLen = std::max(eMax + 3, tune.wpeDelay + tune.wpeTaps + 2);
    Yhist.assign(nch, std::vector<std::vector<cf>>(histLen, std::vector<cf>(K)));
    histPos = 0;
    wpeBins = std::min(K, (int)std::ceil(tune.wpeMaxHz / binHz) + 1);
    tapLags.clear();
    for (int j = 0; j < tune.wpeTaps; ++j) tapLags.push_back(tune.wpeDelay + j);
    appliedLags.clear();
    int L = (int)tapLags.size();
    wpe.assign(nch, std::vector<WpeBin>(wpeBins));
    for (auto& chs : wpe) for (auto& b : chs) {
        b.P.assign(L * L, cd(0)); for (int i = 0; i < L; ++i) b.P[i * L + i] = 1.0;
        b.g.assign(L, cd(0));
    }
    longPow.assign(K, 0.f);
    PY.assign(K, 0.f); PZ = PY; lamY = PY; lamZ = PY;
    lamHistLen = histLen;
    lamZhist.assign(lamHistLen, std::vector<float>(K, 0.f)); lamYhist = lamZhist;
    lamI.assign(K, 0.f); xiPrev.assign(K, 1.f); G.assign(K, 1.f); Gcep = G;
    cepState.assign(K, 0.f); prevPZs.assign(K, 0.f); harm.assign(K, 0.f);
    f0 = 0; voicing = 0;
    for (int b = 0; b < NB; ++b) {
        bandDbPrev[b] = -200.f; bandFloor[b] = -200.f; runLen[b] = 0; runStart[b] = 0; runVals[b].clear();
        rtEst[b].clear(); rt60b[b] = 0.5f;
    }
    rtGlobal = 0.5f;
    ebHist.assign(eMax + 4, std::vector<float>(EB, 0.f)); ebPos = 0;
    eMean.assign(EB, 0.0); eVar.assign(EB, 0.0);
    eCov.assign(eMax + 4, std::vector<double>(EB, 0.0)); eW = 1e-12; eActive = 0;
    rBuf.assign(eMax + 4, 0.f);
    for (int b = 0; b < NB; ++b) { rtEst[b].reserve(48); runVals[b].reserve(4096); }
    for (int i = 0; i < 2; ++i) { echoLag[i] = 0; echoGain2[i] = 0; echoCand[i] = 0; echoCandAge[i] = 0; echoMiss[i] = 0; }
    for (float& e : frameEnergyHist) e = -200.f; fehPos = 0;
    for (int b = 0; b < QB; ++b) { accIn[b] = 0; accOut[b] = 0; eqGain[b] = 1.f; }
    eqBin.assign(K, 1.f); gateG = 1.f; tailCount = 0;
    met = Meters();
}

void DevRevCore::process(float* const* ch, int n) {
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < nch; ++c) {
            inFifo[c][N - hop + fifoPos] = ch[c][i];
            ch[c][i] = outFifo[c][fifoPos];
        }
        if (++fifoPos == hop) {
            processFrame();
            for (int c = 0; c < nch; ++c) {
                std::memmove(inFifo[c].data(), inFifo[c].data() + hop, sizeof(float) * (N - hop));
                std::memcpy(outFifo[c].data(), outAcc[c].data(), sizeof(float) * hop);
                std::memmove(outAcc[c].data(), outAcc[c].data() + hop, sizeof(float) * (N - hop));
                std::fill(outAcc[c].end() - hop, outAcc[c].end(), 0.f);
            }
            fifoPos = 0;
        }
    }
}

// ---------------------------------------------------------------- frame
void DevRevCore::processFrame() {
    for (int c = 0; c < nch; ++c) {
        for (int i = 0; i < N; ++i) frameBuf[i] = inFifo[c][i] * win[i];
        fft.forward(frameBuf.data(), Y[c].data());
    }
    const float amt = prm.dereverb;
    const bool active = amt > 1e-3f;

    if (active && tune.mode != 2) for (int c = 0; c < nch; ++c) wpeChannel(c);
    else for (int c = 0; c < nch; ++c) Z[c] = Y[c];

    analyse();
    if (active && tune.mode != 1) {
        computeGains();
        cepstralSmooth();
        repairGains();
        tailGate();
    } else std::fill(Gcep.begin(), Gcep.end(), 1.f);

    const float wW = std::min(1.f, amt / 0.4f);
    double eIn = 0, eOut = 0;
    for (int c = 0; c < nch; ++c)
        for (int k = 0; k < K; ++k) {
            cf lin = Y[c][k] + wW * (Z[c][k] - Y[c][k]);
            S[c][k] = lin * (Gcep[k]);
        }
    if (active && tune.mode != 1) timbreEq();
    for (int c = 0; c < nch; ++c)
        for (int k = 0; k < K; ++k) {
            S[c][k] *= eqBin[k];
            eIn += pw(Y[c][k]); eOut += pw(S[c][k]);
        }
    met.reductionDb = (float)(10.0 * std::log10((eOut + 1e-20) / (eIn + 1e-20)));

    for (int c = 0; c < nch; ++c) {
        fft.inverse(S[c].data(), frameBuf.data());
        float* acc = outAcc[c].data();
        for (int i = 0; i < N; ++i) acc[i] += frameBuf[i] * win[i] * 0.5f;
    }
    // history rings
    for (int c = 0; c < nch; ++c) Yhist[c][histPos] = Y[c];
    lamZhist[histPos] = lamZ; lamYhist[histPos] = lamY;
    histPos = (histPos + 1) % histLen;
    ++frameCount;
}

// ---------------------------------------------------------------- WPE (online RLS)
void DevRevCore::wpeChannel(int c) {
    const int L = (int)tapLags.size();
    const double alpha = tune.wpeForget;
    auto past = [&](int lag) -> const std::vector<cf>& {
        int idx = (histPos - lag) % histLen; if (idx < 0) idx += histLen; return Yhist[c][idx];
    };
    std::vector<const std::vector<cf>*>& rows = rowsTmp; rows.resize(L);
    for (int j = 0; j < L; ++j) rows[j] = &past(tapLags[j]);

    std::vector<cf>& Zc = Z[c];
    const std::vector<cf>& Yc = Y[c];
    // a-priori estimate
    for (int k = 0; k < wpeBins; ++k) {
        const WpeBin& b = wpe[c][k];
        cd pred = 0;
        for (int j = 0; j < L; ++j) pred += std::conj(b.g[j]) * cd((*rows[j])[k]);
        cd z = cd(Yc[k]) - pred;
        Zc[k] = cf((float)z.real(), (float)z.imag());
    }
    for (int k = wpeBins; k < K; ++k) Zc[k] = Yc[k];

    tmpY.resize(L); tmpPy.resize(L);
    for (int k = 0; k < wpeBins; ++k) {
        WpeBin& b = wpe[c][k];
        float pY = pw(Yc[k]);
        float lp = std::max(longPow[k], 1e-14f);
        // desired-signal variance: local average of a-priori output power
        float lz = 0.5f * pw(Zc[k]);
        lz += 0.25f * pw(Zc[std::max(0, k - 1)]) + 0.25f * pw(Zc[std::min(K - 1, k + 1)]);
        double lam = std::max(lz, lp * 1e-3f);
        bool adapt = pY > lp * 1e-3f && frameCount > 2;
        if (adapt) {
            for (int j = 0; j < L; ++j) tmpY[j] = cd((*rows[j])[k]);
            double quad = 0;
            for (int i = 0; i < L; ++i) {
                cd s = 0; const cd* Pi = &b.P[i * L];
                for (int j = 0; j < L; ++j) s += Pi[j] * tmpY[j];
                tmpPy[i] = s; quad += (std::conj(tmpY[i]) * s).real();
            }
            double den = alpha * lam + quad;
            if (std::isfinite(den) && den > 0) {
                cd e = std::conj(cd(Zc[k]));
                const double inv = 1.0 / den, ia = 1.0 / alpha;
                for (int i = 0; i < L; ++i) {
                    cd ki = tmpPy[i] * inv;
                    b.g[i] += ki * e;
                    cd* Pi = &b.P[i * L];
                    for (int j = 0; j < L; ++j) Pi[j] = (Pi[j] - ki * std::conj(tmpPy[j])) * ia;
                }
                // keep P Hermitian & bounded
                double tr = 0; for (int i = 0; i < L; ++i) tr += b.P[i * L + i].real();
                if (!std::isfinite(tr) || tr > 1e6 * L) {
                    std::fill(b.P.begin(), b.P.end(), cd(0)); for (int i = 0; i < L; ++i) b.P[i * L + i] = 1.0;
                    std::fill(b.g.begin(), b.g.end(), cd(0));
                }
            }
        }
        // safety: prediction must never add energy
        if (pw(Zc[k]) > pY) Zc[k] = Yc[k];
    }
}

void DevRevCore::rebuildWpeTapLayout() {
    std::vector<int> nl;
    for (int j = 0; j < tune.wpeTaps; ++j) nl.push_back(tune.wpeDelay + j);
    for (int l : appliedLags) if (std::find(nl.begin(), nl.end(), l) == nl.end()) nl.push_back(l);
    if (nl == tapLags) return;
    const int Lo = (int)tapLags.size(), Ln = (int)nl.size();
    std::vector<int> map(Ln, -1);
    for (int i = 0; i < Ln; ++i)
        for (int j = 0; j < Lo; ++j) if (tapLags[j] == nl[i]) map[i] = j;
    for (auto& chs : wpe) for (auto& b : chs) {
        std::vector<cd> P(Ln * Ln, cd(0)), g(Ln, cd(0));
        for (int i = 0; i < Ln; ++i) {
            if (map[i] >= 0) g[i] = b.g[map[i]];
            for (int j = 0; j < Ln; ++j) {
                if (map[i] >= 0 && map[j] >= 0) P[i * Ln + j] = b.P[map[i] * Lo + map[j]];
                else if (i == j) P[i * Ln + j] = 1.0;
            }
        }
        b.P.swap(P); b.g.swap(g);
    }
    tapLags = nl;
}

// ---------------------------------------------------------------- analysis
void DevRevCore::analyse() {
    const float inv = 1.f / nch;
    for (int k = 0; k < K; ++k) {
        float py = 0, pz = 0;
        for (int c = 0; c < nch; ++c) { py += pw(Y[c][k]); pz += pw(Z[c][k]); }
        PY[k] = py * inv; PZ[k] = pz * inv;
        lamY[k] = 0.5f * lamY[k] + 0.5f * PY[k];
        lamZ[k] = 0.5f * lamZ[k] + 0.5f * PZ[k];
        const float a = frameCount < 50 ? 0.9f : 0.998f;
        longPow[k] = a * longPow[k] + (1 - a) * PY[k];
    }
    updateRt60();
    estimatePitch();

    // onset-after-pause detector (broadband 200-4000 Hz)
    int k0 = (int)(200 / binHz), k1 = std::min(K - 1, (int)(4000 / binHz));
    double e = 0; for (int k = k0; k <= k1; ++k) e += PY[k];
    float edb = (float)(10 * std::log10(e + 1e-20));
    float prevMax = -200.f;
    for (int i = 4; i < 20; ++i) prevMax = std::max(prevMax, frameEnergyHist[(fehPos - i + 32) % 32]);
    onsetAfterPause = edb > prevMax + 12.f;
    frameEnergyHist[fehPos] = edb; fehPos = (fehPos + 1) % 32;
    if (prm.echo) updateEcho();
    else if (!appliedLags.empty() || echoLag[0] || echoLag[1]) {
        echoLag[0] = echoLag[1] = 0; echoGain2[0] = echoGain2[1] = 0;
        appliedLags.clear(); rebuildWpeTapLayout();
    }
}

void DevRevCore::updateRt60() {
    const float frameSec = (float)hop / (float)fs;
    const int minRun = std::max(6, (int)std::round(0.05f / frameSec));
    for (int b = 0; b < NB; ++b) {
        int a = std::max(1, (int)(bandLo[b] / binHz)), z = std::min(K - 1, (int)(bandHi[b] / binHz));
        if (z <= a) continue;
        double e = 0, lt = 0;
        for (int k = a; k <= z; ++k) { e += PY[k]; lt += longPow[k]; }
        float db = (float)(10 * std::log10(e + 1e-20));
        float ltdb = (float)(10 * std::log10(lt + 1e-20));
        float sm = bandDbPrev[b] < -150.f ? db : 0.5f * bandDbPrev[b] + 0.5f * db;
        // noise-floor tracker (minimum statistics with slow upward drift)
        if (bandFloor[b] < -150.f || sm < bandFloor[b]) bandFloor[b] = sm; else bandFloor[b] += 0.02f;
        // a decay run starts on a fall and continues while the level stays within 1.5 dB of its running minimum
        bool cont;
        if (runLen[b] == 0) {
            cont = sm < bandDbPrev[b] - 0.02f;
            if (cont) { runVals[b].clear(); runVals[b].push_back(bandDbPrev[b]); runStart[b] = sm; }
        } else cont = sm <= runStart[b] + 1.5f;
        if (cont) {
            runVals[b].push_back(sm); ++runLen[b]; runStart[b] = std::min(runStart[b], sm);
        } else if (runLen[b] > 0) {
            auto& v = runVals[b];
            // trim the trailing bump back to the run minimum
            size_t im = (size_t)(std::min_element(v.begin(), v.end()) - v.begin()); v.resize(im + 1);
            float drop = v.front() - v.back();
            const int W = std::max(5, (int)std::round(0.045f / frameSec));
            if ((int)v.size() >= W + 2 && drop >= 8.f && v.front() > ltdb - 6.f) {
                // The start of a free decay is the voice's own (fast) release; the reverberant tail is the
                // slowest part above the noise floor. Fit LS slopes on sliding windows and keep the shallowest.
                double slope = 0; bool ok = false;
                int s0 = 0; while (s0 < (int)v.size() && v[s0] > v.front() - 5.f) ++s0;
                int s1 = (int)v.size() - 1; while (s1 > s0 && v[s1] < bandFloor[b] + 12.f) --s1;
                int n = s1 - s0 + 1;
                if (n >= W && v[s0] - v[s1] >= 6.f) {
                    double sx = 0, sy = 0, sxx = 0, sxy = 0;
                    for (int i = 0; i < n; ++i) { double x = i, y = v[s0 + i]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
                    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx + 1e-12);
                    ok = slope < -1e-3;
                }
                if (ok) {
                    float rt = (float)(-60.0 / slope * frameSec);
                    rt = std::min(3.f, std::max(0.1f, rt));
                    if (rtEst[b].size() >= 48) { std::rotate(rtEst[b].begin(), rtEst[b].begin() + 1, rtEst[b].end()); rtEst[b].back() = rt; }
                    else rtEst[b].push_back(rt);
                    if (rtEst[b].size() >= 4) {
                        std::vector<float>& s = sortBuf; s.assign(rtEst[b].begin(), rtEst[b].end()); std::sort(s.begin(), s.end());
                        rt60b[b] = s[(size_t)(rtQuant * (s.size() - 1))];
                    }
                }
            }
            runLen[b] = 0;
        }
        bandDbPrev[b] = sm;
    }
    // pooled estimate from 1-11 kHz bands (speech harmonics make low bands unreliable)
    std::vector<float>& pool = poolBuf; pool.clear();
    for (int b = 3; b <= 6; ++b) pool.insert(pool.end(), rtEst[b].begin(), rtEst[b].end());
    if (pool.size() >= 6) {
        std::sort(pool.begin(), pool.end());
        float target = pool[(size_t)(rtQuant * (pool.size() - 1))];
        rtGlobal = 0.9f * rtGlobal + 0.1f * target;
    }
    met.rt60 = rtGlobal;
}

void DevRevCore::updateEcho() {
    // band powers
    std::vector<float>& cur = ebHist[ebPos];
    const float lo = 200.f, hi = std::min(6000.f, (float)fs * 0.45f);
    for (int b = 0; b < EB; ++b) {
        float fl = lo * std::pow(hi / lo, (float)b / EB), fh = lo * std::pow(hi / lo, (float)(b + 1) / EB);
        int a = (int)(fl / binHz), z = std::max(a, (int)(fh / binHz));
        double e = 0; for (int k = a; k <= z && k < K; ++k) e += PY[k];
        cur[b] = (float)std::sqrt(e); // magnitude envelope: less dominated by peaks
    }
    const int H = (int)ebHist.size();
    const double rho = 1.0 - (double)hop / (fs * 3.0);
    // statistics are only accumulated on frames that carry voice-like activity
    double fe = 0, fl = 0;
    for (int k = (int)(200 / binHz); k < std::min(K, (int)(6000 / binHz)); ++k) { fe += PY[k]; fl += longPow[k]; }
    const bool act = fe > 0.1 * fl && fe > 1e-12;
    if (act) {
        eW = rho * eW + (1 - rho);
        for (int b = 0; b < EB; ++b) {
            eMean[b] = rho * eMean[b] + (1 - rho) * cur[b];
            eVar[b] = rho * eVar[b] + (1 - rho) * (double)cur[b] * cur[b];
        }
        for (int d = eMin - 12; d <= eMax + 3; ++d) {
            if (d < 1) continue;
            const std::vector<float>& pst = ebHist[(ebPos - d + H * 4) % H];
            for (int b = 0; b < EB; ++b) eCov[d][b] = rho * eCov[d][b] + (1 - rho) * (double)cur[b] * pst[b];
        }
        ++eActive;
    }
    ebPos = (ebPos + 1) % H;

    const double warm = 1.5 * fs / hop;
    if (eActive < warm || (frameCount % 8) != 0) return;

    std::vector<float>& r = rBuf; std::fill(r.begin(), r.end(), 0.f);
    for (int d = std::max(1, eMin - 4); d <= eMax + 3; ++d) {
        double s = 0; int cnt = 0;
        for (int b = 0; b < EB; ++b) {
            double m = eMean[b] / eW, v = eVar[b] / eW - m * m;
            if (v <= 1e-20 || v < 0.05 * m * m) continue;   // (near-)constant envelope: no information
            s += (eCov[d][b] / eW - m * m) / v; ++cnt;
        }
        if (cnt < EB / 2) s = 0;
        r[d] = cnt ? (float)(s / cnt) : 0.f;
    }
    struct Cand { int d; float sharp; float g2; };
    Cand cands_[64]; int nc = 0;
    auto baseAt = [&](int d) {
        double s = 0; int c = 0;
        for (int o = 7; o <= 12; ++o) {
            if (d - o >= 1) { s += r[d - o]; ++c; }
            if (d + o < (int)r.size()) { s += r[d + o]; ++c; }
        }
        return c ? (float)(s / c) : 0.f;
    };
    for (int d = eMin; d <= eMax - 9; ++d) {
        if (r[d] < r[d - 1] || r[d] < r[d + 1]) continue;
        float sharp = r[d] - baseAt(d);
        if (sharp > echoThresh && r[d] > 0.1f && nc < 64) cands_[nc++] = {d, sharp, std::min(1.f, std::max(0.f, sharp * 2.5f))};
    }
    std::sort(cands_, cands_ + nc, [](const Cand& a, const Cand& b) { return a.sharp > b.sharp; });
    if (nc > 2) nc = 2;
    struct CR { Cand* b; Cand* e; Cand* begin() const { return b; } Cand* end() const { return e; } } cands{cands_, cands_ + nc};

    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        // is the active echo still supported?
        if (echoLag[i]) {
            bool found = false;
            for (auto& c : cands) if (std::abs(c.d - echoLag[i]) <= 1) { found = true; echoGain2[i] = 0.8f * echoGain2[i] + 0.2f * c.g2; echoLag[i] = c.d == echoLag[i] ? echoLag[i] : echoLag[i]; }
            if (!found) { if (++echoMiss[i] > 12) { echoLag[i] = 0; echoGain2[i] = 0; echoMiss[i] = 0; changed = true; } }
            else echoMiss[i] = 0;
        }
    }
    for (auto& c : cands) {
        bool used = false;
        for (int i = 0; i < 2; ++i) if (echoLag[i] && std::abs(c.d - echoLag[i]) <= 1) used = true;
        if (used) continue;
        // candidate confirmation (needs to persist ~3 evaluations)
        int slot = -1;
        for (int i = 0; i < 2; ++i) if (echoCand[i] && std::abs(echoCand[i] - c.d) <= 1) slot = i;
        if (slot < 0) { for (int i = 0; i < 2; ++i) if (!echoCand[i]) { slot = i; echoCand[i] = c.d; echoCandAge[i] = 0; break; } }
        if (slot < 0) continue;
        if (++echoCandAge[slot] >= 3) {
            for (int i = 0; i < 2; ++i) if (!echoLag[i]) { echoLag[i] = c.d; echoGain2[i] = c.g2; changed = true; break; }
            echoCand[slot] = 0; echoCandAge[slot] = 0;
        }
    }
    // stale candidates decay
    for (int i = 0; i < 2; ++i) if (echoCand[i]) {
        bool seen = false; for (auto& c : cands) if (std::abs(c.d - echoCand[i]) <= 1) seen = true;
        if (!seen) { echoCand[i] = 0; echoCandAge[i] = 0; }
    }
    if (changed) {
        appliedLags.clear();
        for (int i = 0; i < 2; ++i) if (echoLag[i])
            for (int dd = -1; dd <= 1; ++dd) appliedLags.push_back(echoLag[i] + dd);
        std::sort(appliedLags.begin(), appliedLags.end());
        appliedLags.erase(std::unique(appliedLags.begin(), appliedLags.end()), appliedLags.end());
        if (tune.mode != 2) rebuildWpeTapLayout();
    }
    for (int i = 0; i < 2; ++i) {
        met.echoDelayMs[i] = echoLag[i] ? (float)(1000.0 * echoLag[i] * hop / fs) : 0.f;
        met.echoStrength[i] = echoGain2[i];
    }
}

void DevRevCore::estimatePitch() {
    const int kMax = std::min(K - 1, (int)(4000 / binHz));
    for (int k = 0; k < K; ++k) specTmp[k] = cf(k <= kMax ? std::pow(PZ[k] + 1e-20f, 0.8f) : 0.f, 0.f);
    specTmp[0] = cf(0, 0);
    fft.inverse(specTmp.data(), acBuf.data());
    const float r0 = acBuf[0] / std::max(winAc[0], kEps);
    voicing = 0; f0 = 0;
    std::fill(harm.begin(), harm.end(), 0.f);
    double lt = 0, e = 0; for (int k = 2; k <= kMax; ++k) { e += PZ[k]; lt += longPow[k]; }
    if (r0 <= 0 || e < lt * 0.05) { met.voiced = 0; met.f0 = 0; return; }
    int tMin = (int)(fs / 600.0), tMax = std::min((int)(fs / 70.0), (int)(0.6 * N));
    std::vector<float>& rn = rnBuf; std::fill(rn.begin(), rn.end(), 0.f);
    float best = 0; int bt = 0;
    for (int t = tMin - 1; t <= tMax + 1; ++t) {
        rn[std::min(t, tMax + 1)] = (acBuf[t] / std::max(winAc[t], kEps)) / r0;
    }
    for (int t = tMin; t <= tMax; ++t) if (rn[t] > best && rn[t] >= rn[t - 1] && rn[t] >= rn[t + 1]) { best = rn[t]; bt = t; }
    if (bt == 0) { met.voiced = 0; return; }
    // octave-error guard: prefer the shortest lag with a comparable peak
    for (int t = tMin; t < bt; ++t)
        if (rn[t] > 0.9f * best && rn[t] >= rn[t - 1] && rn[t] >= rn[t + 1]) { bt = t; best = rn[t]; break; }
    float a = rn[bt - 1], b = rn[bt], c = rn[bt + 1];
    float den = a - 2 * b + c; float off = std::fabs(den) > 1e-9f ? 0.5f * (a - c) / den : 0.f;
    off = std::max(-0.5f, std::min(0.5f, off));
    f0 = (float)(fs / (bt + off));
    voicing = std::max(0.f, std::min(1.f, (best - 0.35f) / 0.5f));
    met.voiced = voicing; met.f0 = voicing > 0 ? f0 : 0.f;
    if (voicing <= 0) return;
    const float fMax = 5000.f;
    for (int h = 1; h * f0 < fMax; ++h) {
        float fc = h * f0, sig = std::max(binHz * 0.6f, 0.012f * h * f0) ;
        int ka = std::max(1, (int)((fc - 3 * sig) / binHz)), kb = std::min(K - 1, (int)((fc + 3 * sig) / binHz) + 1);
        for (int k = ka; k <= kb; ++k) {
            float d = (k * binHz - fc) / sig;
            harm[k] = std::max(harm[k], std::exp(-0.5f * d * d));
        }
    }
}

// ---------------------------------------------------------------- gains
void DevRevCore::computeGains() {
    const float a = prm.dereverb;
    const float frameSec = (float)hop / (float)fs;
    const int Lc = tune.wpeDelay + tune.wpeTaps;
    const bool wpeOn = tune.mode != 2;
    float Tl = tune.lateBoundary > 0 ? tune.lateBoundary : (wpeOn ? Lc * frameSec : 0.05f);
    int Nl = std::max(1, (int)std::round(Tl / frameSec));
    Nl = std::min(Nl, lamHistLen - 1);
    auto lamAt = [&](int lag) -> const std::vector<float>& {
        int idx = (histPos - lag) % lamHistLen; if (idx < 0) idx += lamHistLen; return lamZhist[idx];
    };
    auto lamYAt = [&](int lag) -> const std::vector<float>& {
        int idx = (histPos - lag) % lamHistLen; if (idx < 0) idx += lamHistLen; return lamYhist[idx];
    };
    const std::vector<float>& lz = tune.lateFromY ? lamYAt(Nl) : lamAt(Nl);
    const float mu = std::min(2.f, std::max(0.f, (a - 0.25f) / 0.75f));
    const float floorDb = -std::min(a, 1.f) * (6.f + 34.f * (1.f - prm.ambience)) - std::max(0.f, a - 1.f) * 10.f;
    const float gFloor = std::pow(10.f, floorDb / 20.f);
    const float beta = tune.ddBeta;

    // per-bin decay constant from band RT60s
    for (int k = 0; k < K; ++k) {
        float f = k * binHz;
        int b = 0; while (b < NB - 1 && f > bandHi[b]) ++b;
        (void)b; float rt = std::max(0.15f, rtGlobal);
        float delta = 3.f * 2.302585f / rt;
        float late = tune.lateMu * std::exp(-2.f * delta * Nl * frameSec) * lz[k];
        if (tune.lateFromY && wpeOn) late = std::max(late - std::max(0.f, lamY[k] - lamZ[k]), late * tune.residFloor);
        float echo = 0;
        for (int i = 0; i < 2; ++i) if (echoLag[i] && echoLag[i] < lamHistLen) {
            float resid = wpeOn ? 0.15f : 1.f;
            echo += resid * echoGain2[i] * lamAt(echoLag[i])[k];
        }
        lamI[k] = late + echo + longPow[k] * 1e-6f + 1e-20f;
        float gam = PZ[k] / (mu * lamI[k] + 1e-20f);
        float xi = beta * xiPrev[k] + (1 - beta) * std::max(gam - 1.f, 0.f);
        xi = std::max(xi, 1e-3f);
        float g = mu > 0 ? xi / (1.f + xi) : 1.f;
        g = std::max(g, gFloor);
        G[k] = g;
        xiPrev[k] = std::min(1e4f, g * g * gam);
    }
}

void DevRevCore::cepstralSmooth() {
    const float a = prm.dereverb;
    const float floorDb = -std::min(a, 1.f) * (6.f + 34.f * (1.f - prm.ambience)) - std::max(0.f, a - 1.f) * 10.f;
    const float gFloor = std::pow(10.f, floorDb / 20.f);
    for (int k = 0; k < K; ++k) logG[k] = std::log(std::max(G[k], 1e-4f));
    for (int k = 1; k < K - 1; ++k) logG[N - k] = logG[k];
    std::vector<cf>& C = specTmp;
    fft.forward(logG.data(), C.data());          // real, even -> real cepstrum (scaled by N)
    const int qEnv = std::max(3, (int)std::round(0.0004 * fs));
    int q0 = voicing > 0.2f && f0 > 0 ? (int)std::round(fs / f0) : -100;
    for (int q = 0; q < K; ++q) {
        float al;
        if (q < qEnv) al = 0.0f;
        else if (std::abs(q - q0) <= 2) al = 0.4f;
        else al = 0.92f;
        cepState[q] = al * cepState[q] + (1 - al) * C[q].real();
        C[q] = cf(cepState[q], 0.f);
    }
    fft.inverse(C.data(), cep.data());
    for (int k = 0; k < K; ++k) Gcep[k] = std::min(1.f, std::max(gFloor, std::exp(cep[k])));
}

void DevRevCore::repairGains() {
    const float r = prm.repair;
    if (r <= 0) return;
    for (int k = 0; k < K; ++k) {
        float g = Gcep[k];
        // onset protection: sharp rises above the decaying hold are direct sound
        float hold = prevPZs[k];
        float rise = PZ[k] / (hold + 1e-20f);
        if (rise > 3.f && PZ[k] > longPow[k] * 0.01f) g = std::max(g, r * std::min(1.f, (rise - 1.f) / 4.f));
        prevPZs[k] = std::max(PZ[k], hold * 0.6f);
        // harmonic protection on voiced frames, weighted by direct-sound dominance
        if (voicing > 0 && harm[k] > 0.05f) {
            float p = PZ[k] / (PZ[k] + lamI[k]);
            float h = r * 0.85f * harm[k] * voicing * std::min(1.f, 1.6f * p);
            g = std::max(g, h);
        }
        Gcep[k] = std::min(1.f, g);
    }
}

void DevRevCore::tailGate() {
    // Frame-level suppression of pure reverb tails (pauses): if almost no bin carries
    // energy beyond the reverb prediction and there is no pitch, the frame is tail only.
    const float a = prm.dereverb;
    if (!tune.gate || a <= 0.05f) { gateG = 1.f; return; }
    int k0 = std::max(1, (int)(100 / binHz)), k1 = std::min(K - 1, (int)(8000 / binHz));
    double tot = 0, dir = 0;
    for (int k = k0; k <= k1; ++k) {
        tot += PZ[k];
        if (PZ[k] > 2.5f * lamI[k]) dir += PZ[k];
    }
    float dfrac = tot > 0 ? (float)(dir / tot) : 0.f;
    bool tail = dfrac < 0.35f && voicing < 0.15f;
    tailCount = tail ? tailCount + 1 : 0;
    float depthDb = -(10.f + 16.f * (1.f - prm.ambience)) * std::min(a, 1.5f);
    float target = tailCount >= 2 ? std::pow(10.f, depthDb / 20.f) : 1.f;
    if (target < gateG) {
        const float c = std::exp(-(float)hop / (float)(fs * 0.035));
        gateG = c * gateG + (1 - c) * target;
    } else gateG = target;
    if (gateG < 0.999f) for (int k = 0; k < K; ++k) Gcep[k] *= gateG;
}

void DevRevCore::timbreEq() {
    // Measure how much clean (high direct-to-interference) energy the gains removed per band
    // and give it back. Only boosts, max +6 dB, slow.
    const float r = prm.repair;
    for (int b = 0; b < QB; ++b) {
        int a = std::max(1, (int)(qLo[b] / binHz)), z = std::min(K - 1, (int)(qHi[b] / binHz));
        double in = 0, out = 0;
        for (int k = a; k <= z; ++k) {
            if (PZ[k] > 4.f * lamI[k] && PZ[k] > longPow[k] * 0.03f) {
                in += PZ[k];
                float g = Gcep[k];
                out += (double)g * g * PZ[k];
            }
        }
        if (in > 0) { accIn[b] = 0.997 * accIn[b] + in; accOut[b] = 0.997 * accOut[b] + out; }
        float target = accOut[b] > 0 ? (float)std::sqrt(accIn[b] / accOut[b]) : 1.f;
        target = std::min(2.f, std::max(1.f, target));
        target = 1.f + r * (target - 1.f);
        eqGain[b] = 0.98f * eqGain[b] + 0.02f * target;
    }
    // interpolate band gains to bins (log-frequency)
    for (int k = 0; k < K; ++k) {
        float f = std::max(1.f, k * binHz);
        if (f <= std::sqrt(qLo[0] * qHi[0])) { eqBin[k] = eqGain[0]; continue; }
        int b = 0;
        while (b < QB - 1 && f > std::sqrt(qLo[b + 1] * qHi[b + 1])) ++b;
        if (b >= QB - 1) { eqBin[k] = eqGain[QB - 1]; continue; }
        float c0 = std::log(std::sqrt(qLo[b] * qHi[b])), c1 = std::log(std::sqrt(qLo[b + 1] * qHi[b + 1]));
        float t = (std::log(f) - c0) / (c1 - c0);
        eqBin[k] = eqGain[b] + t * (eqGain[b + 1] - eqGain[b]);
    }
}

} // namespace devrev

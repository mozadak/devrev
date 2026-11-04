// Offline test harness: devrev_cli in.wav out.wav [key=value ...]
#include "../core/DevRevCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

struct Wav { int sr = 48000, ch = 1; std::vector<float> d; };

static bool readWav(const char* p, Wav& w) {
    FILE* f = fopen(p, "rb"); if (!f) return false;
    char id[4]; uint32_t sz; fread(id, 1, 4, f); fread(&sz, 4, 1, f); fread(id, 1, 4, f);
    int fmt = 1, bits = 16;
    while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(sz); fread(b.data(), 1, sz, f);
            fmt = b[0] | (b[1] << 8); w.ch = b[2] | (b[3] << 8);
            w.sr = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24); bits = b[14] | (b[15] << 8);
            if (fmt == 0xFFFE) fmt = b[24] | (b[25] << 8);
        } else if (!memcmp(id, "data", 4)) {
            int bps = bits / 8; size_t n = sz / bps; std::vector<uint8_t> b(sz); fread(b.data(), 1, sz, f);
            w.d.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* q = &b[i * bps];
                if (fmt == 3 && bits == 32) { float v; memcpy(&v, q, 4); w.d[i] = v; }
                else if (bits == 16) w.d[i] = (int16_t)(q[0] | (q[1] << 8)) / 32768.f;
                else if (bits == 24) { int32_t v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); w.d[i] = (v >> 8) / 8388608.f; }
                else if (bits == 32) { int32_t v; memcpy(&v, q, 4); w.d[i] = v / 2147483648.f; }
            }
            break;
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    fclose(f); return !w.d.empty();
}

static void writeWav(const char* p, const Wav& w) {
    FILE* f = fopen(p, "wb");
    uint32_t data = (uint32_t)(w.d.size() * 4), riff = 36 + data;
    uint16_t fmt = 3, ch = (uint16_t)w.ch, bits = 32, ba = (uint16_t)(4 * w.ch);
    uint32_t sr = w.sr, br = sr * ba, fsz = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fsz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(w.d.data(), 4, w.d.size(), f); fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: devrev_cli in.wav out.wav [dereverb= repair= ambience= echo= mode= taps= delay= forget= latemu=]\n"); return 1; }
    Wav in; if (!readWav(argv[1], in)) { fprintf(stderr, "read fail\n"); return 1; }
    devrev::DevRevCore core; devrev::Params p;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i]; auto e = a.find('='); if (e == std::string::npos) continue;
        std::string k = a.substr(0, e); double v = atof(a.c_str() + e + 1);
        if (k == "dereverb") p.dereverb = (float)v; else if (k == "repair") p.repair = (float)v;
        else if (k == "ambience") p.ambience = (float)v; else if (k == "echo") p.echo = v != 0;
        else if (k == "mode") core.tune.mode = (int)v; else if (k == "taps") core.tune.wpeTaps = (int)v;
        else if (k == "delay") core.tune.wpeDelay = (int)v; else if (k == "forget") core.tune.wpeForget = v;
        else if (k == "latemu") core.tune.lateMu = (float)v; else if (k == "dd") core.tune.ddBeta = (float)v;
        else if (k == "rtq") core.rtQuant = (float)v;
        else if (k == "latey") core.tune.lateFromY = v != 0;
        else if (k == "rfloor") core.tune.residFloor = (float)v;
        else if (k == "lateb") core.tune.lateBoundary = (float)v;
        else if (k == "gate") core.tune.gate = v != 0;
        else if (k == "wpehz") core.tune.wpeMaxHz = (float)v;
    }
    core.prepare(in.sr, in.ch); core.setParams(p);
    const int nch = std::min(2, in.ch); const size_t frames = in.d.size() / in.ch;
    const int L = core.getLatency();
    std::vector<std::vector<float>> buf(nch, std::vector<float>(frames + L, 0.f));
    for (size_t i = 0; i < frames; ++i) for (int c = 0; c < nch; ++c) buf[c][i] = in.d[i * in.ch + c];
    auto t0 = std::chrono::steady_clock::now();
    const int B = 480; std::vector<float*> ptr(nch);
    double sumRt = 0; int nRt = 0;
    for (size_t pos = 0; pos < frames + L; pos += B) {
        int n = (int)std::min<size_t>(B, frames + L - pos);
        for (int c = 0; c < nch; ++c) ptr[c] = buf[c].data() + pos;
        core.process(ptr.data(), n);
        if (pos % (B * 100) == 0) { sumRt += core.meters().rt60; ++nRt; }
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Wav out; out.sr = in.sr; out.ch = nch; out.d.resize(frames * nch);
    for (size_t i = 0; i < frames; ++i) for (int c = 0; c < nch; ++c) out.d[i * nch + c] = buf[c][i + L];
    writeWav(argv[2], out);
    const auto& m = core.meters();
    fprintf(stderr, "latency=%d rt60=%.2f echo=[%.0fms %.2f, %.0fms %.2f] rtf=%.3f\n", L, m.rt60,
            m.echoDelayMs[0], m.echoStrength[0], m.echoDelayMs[1], m.echoStrength[1], sec / (frames / (double)in.sr));
    return 0;
}

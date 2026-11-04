// DevRev - minimal real FFT (radix-2, iterative). Original code, no dependencies.
#pragma once
#include <vector>
#include <complex>
#include <cmath>

namespace devrev {

using cf = std::complex<float>;

class RealFFT {
public:
    void init(int n) {
        N = n; H = n / 2;
        tw.resize(H / 2 + 1);
        for (int i = 0; i < (int)tw.size(); ++i) {
            double a = -2.0 * M_PI * i / H;
            tw[i] = cf((float)std::cos(a), (float)std::sin(a));
        }
        post.resize(H + 1);
        for (int k = 0; k <= H; ++k) {
            double a = -2.0 * M_PI * k / N;
            post[k] = cf((float)std::cos(a), (float)std::sin(a));
        }
        rev.resize(H);
        int bits = 0; while ((1 << bits) < H) ++bits;
        for (int i = 0; i < H; ++i) {
            int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[i] = r;
        }
        buf.resize(H);
    }
    int size() const { return N; }

    // x: N real samples -> X: N/2+1 bins (unnormalised)
    void forward(const float* x, cf* X) {
        for (int i = 0; i < H; ++i) buf[rev[i]] = cf(x[2 * i], x[2 * i + 1]);
        fftInPlace(buf.data(), false);
        X[0] = cf(buf[0].real() + buf[0].imag(), 0.f);
        X[H] = cf(buf[0].real() - buf[0].imag(), 0.f);
        for (int k = 1; k < H; ++k) {
            cf a = buf[k], b = std::conj(buf[H - k]);
            cf e = 0.5f * (a + b);
            cf o = cf(0.f, -0.5f) * (a - b);
            X[k] = e + post[k] * o;
        }
    }
    // X: N/2+1 bins -> x: N real samples, scaled so inverse(forward(x)) == x
    void inverse(const cf* X, float* x) {
        std::vector<cf>& z = tmp; z.resize(H);
        for (int k = 0; k < H; ++k) {
            cf a = X[k], b = std::conj(X[H - k]);
            cf e = 0.5f * (a + b);
            cf o = 0.5f * (a - b) * std::conj(post[k]);
            z[k] = e + cf(0.f, 1.f) * o;
        }
        for (int i = 0; i < H; ++i) buf[rev[i]] = std::conj(z[i]);
        fftInPlace(buf.data(), false);
        const float s = 1.0f / H;
        for (int i = 0; i < H; ++i) { cf v = std::conj(buf[i]) * s; x[2 * i] = v.real(); x[2 * i + 1] = v.imag(); }
    }

private:
    void fftInPlace(cf* a, bool) {
        for (int len = 2; len <= H; len <<= 1) {
            int half = len >> 1, step = H / len;
            for (int i = 0; i < H; i += len) {
                for (int j = 0; j < half; ++j) {
                    cf w = twiddle(j * step);
                    cf u = a[i + j], v = a[i + j + half] * w;
                    a[i + j] = u + v; a[i + j + half] = u - v;
                }
            }
        }
    }
    inline cf twiddle(int idx) const { return tw[idx]; } // exp(-2*pi*i*idx/H), idx < H/2
    int N = 0, H = 0;
    std::vector<cf> tw, post, buf, tmp;
    std::vector<int> rev;
};

} // namespace devrev

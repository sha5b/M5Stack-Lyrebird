// See syrinx.h. Constants and formulas follow syrinx-processor.js / docs/08 §8.1.
#include "syrinx.h"
#include "calibration.h"
#include <math.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#define MAX_CONTOUR 8

static const float CALIBRATION_GAMMA = 24000.0f; // gamma the offline table was measured at
static const float PHI_AT_BETA_ONE = 0.17061f;   // measured f0/gamma at alpha 0.1, beta 1
static const float DETUNE_TWO = 1.0040348f;      // +7 cents: the second syringeal side
#define FILTER_UPDATE 32                          // samples between tract retunes
static const float DEFAULT_ATTACK = 0.008f;
static const float RELEASE_S = 0.006f;
static const float DECAY_TAU_DIVISOR = 2.5f;
static const float HOLD_FRACTION = 0.75f;
static const float SOURCE_GAIN = 0.35f;
static const float ODE_X0 = 1.0e-4f;

// timbre classes: {gain, q}
static const float TIMBRE_GAIN[3] = {1.0f, 1.25f, 1.75f};
static const float TIMBRE_Q[3] = {9.0f, 5.0f, 3.0f};

static float s_sr = 22050.0f;
static float s_dcCoeff;  // 180 Hz one-pole (source HP and output DC block)
static float s_lpCoeff;  // 12 kHz one-pole lowpass

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void setBandpass(float freq, float q) {
        float f = fminf(fmaxf(freq, 40.0f), s_sr * 0.45f);
        float w0 = 2.0f * (float)M_PI * f / s_sr;
        float alpha = sinf(w0) / (2.0f * fmaxf(q, 0.3f));
        float a0 = 1.0f + alpha;
        b0 = alpha / a0;
        b1 = 0;
        b2 = -alpha / a0;
        a1 = -2.0f * cosf(w0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

    void reset() { x1 = x2 = y1 = y2 = 0; }
};

struct Voice {
    volatile bool active = false;
    const SyllableData* syl = nullptr;
    // voicing applied at note start
    float dur = 0;       // after durScale
    float level = 0;     // after levelScale
    float harmonic = 0;
    float am = 0, amDepth = 0;
    float vibRate = 0, vibDepth = 0;
    float gain = 1;
    // contour cache: log(hz * pitchMul) per point, plus segment cursor
    uint8_t nContour = 0;
    float contT[MAX_CONTOUR];
    float contLog[MAX_CONTOUR];
    uint8_t seg = 0;

    int32_t pos = 0;       // samples since onset
    int32_t durSamples = 0;
    int32_t envN = 0;

    float gamma = CALIBRATION_GAMMA;
    int oversample = 16;
    float dt = 1.0f / (22050.0f * 16.0f);
    float x[2] = {ODE_X0, ODE_X0};
    float y[2] = {0, 0};
    Biquad tract[2];
    Biquad tractHarm[2];
    float srcHpX[2] = {0, 0}, srcHpY[2] = {0, 0};
    float dcX = 0, dcY = 0, lpY = 0;
    float env = 0;
};

static Voice s_voices[SYRINX_MAX_VOICES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_lastLevel = 0;

void syrinxInit(float sampleRate) {
    s_sr = sampleRate;
    s_dcCoeff = expf(-2.0f * (float)M_PI * 180.0f / s_sr);
    s_lpCoeff = 1.0f - expf(-2.0f * (float)M_PI * 12000.0f / s_sr);
    syrinxPanic();
}

void syrinxPanic() {
    portENTER_CRITICAL(&s_mux);
    for (auto& v : s_voices) v.active = false;
    portEXIT_CRITICAL(&s_mux);
}

static float gammaForContour(const SyllableData* syl) {
    float logSum = 0;
    for (int i = 0; i < syl->nContour; i++) logSum += logf(fmaxf(80.0f, syl->contour[i].hz));
    float reference = expf(logSum / fmaxf(1, syl->nContour));
    return fminf(150000.0f, fmaxf(1200.0f, reference / PHI_AT_BETA_ONE));
}

static int oversampleForGamma(float gamma) {
    int n = (int)ceilf(gamma / (0.06f * s_sr));
    return n < 16 ? 16 : (n > 64 ? 64 : n);
}

static inline float clampBeta(float beta) {
    return fminf(CAL_BETA_MAX, fmaxf(CAL_BETA_MIN, beta));
}

// beta for a target f0 via the offline table (bilinear over pressure x f0).
static float betaForF0(float targetF0, float pressure, float gamma) {
    float f0 = targetF0 * CALIBRATION_GAMMA / gamma;
    // pressure row (linear scan over 29 entries, as in the worklet)
    int pi = 0;
    while (pi < CAL_PRESSURE_GRID_LEN - 2 && CAL_PRESSURE_GRID[pi + 1] < pressure) pi++;
    float pSpan = CAL_PRESSURE_GRID[pi + 1] - CAL_PRESSURE_GRID[pi];
    if (pSpan == 0) pSpan = 1;
    float pFrac = fminf(1.0f, fmaxf(0.0f, (pressure - CAL_PRESSURE_GRID[pi]) / pSpan));

    // f0 column (binary search over the 192-entry grid)
    float clamped = fminf(fmaxf(f0, CAL_INV_F0_GRID[0]), CAL_INV_F0_GRID[CAL_INV_F0_GRID_LEN - 1]);
    int lo = 0, hi = CAL_INV_F0_GRID_LEN - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (CAL_INV_F0_GRID[mid] <= clamped) lo = mid; else hi = mid;
    }
    float fSpan = CAL_INV_F0_GRID[hi] - CAL_INV_F0_GRID[lo];
    if (fSpan == 0) fSpan = 1;
    float fFrac = (clamped - CAL_INV_F0_GRID[lo]) / fSpan;

    const float* rowA = CAL_INV_BETA[pi];
    const float* rowB = CAL_INV_BETA[pi + 1 < 29 ? pi + 1 : 28];
    float a = rowA[lo] + (rowA[hi] - rowA[lo]) * fFrac;
    float b = rowB[lo] + (rowB[hi] - rowB[lo]) * fFrac;
    float beta = a + (b - a) * pFrac;
    if (beta <= 0) {
        beta = f0 / (PHI_AT_BETA_ONE * CALIBRATION_GAMMA);
        beta = beta * beta;
    }
    return clampBeta(beta);
}

// peak-to-peak swing of y/gamma, divided out of the source so loudness
// follows the envelope instead of the pitch (§8.1).
static float ampPP(float alpha, float beta, float pressure) {
    int pi = 0;
    while (pi < CAL_PRESSURE_GRID_LEN - 2 && CAL_PRESSURE_GRID[pi + 1] < pressure) pi++;
    float pSpan = CAL_PRESSURE_GRID[pi + 1] - CAL_PRESSURE_GRID[pi];
    if (pSpan == 0) pSpan = 1;
    float pFrac = fminf(1.0f, fmaxf(0.0f, (pressure - CAL_PRESSURE_GRID[pi]) / pSpan));

    int bi = 0;
    while (bi < CAL_BETA_GRID_LEN - 2 && CAL_BETA_GRID[bi + 1] < beta) bi++;
    float bSpan = CAL_BETA_GRID[bi + 1] - CAL_BETA_GRID[bi];
    if (bSpan == 0) bSpan = 1;
    float bFrac = fminf(1.0f, fmaxf(0.0f, (beta - CAL_BETA_GRID[bi]) / bSpan));

    const float* rowA = CAL_AMP_PP[pi];
    const float* rowB = CAL_AMP_PP[pi + 1 < 29 ? pi + 1 : 28];
    float a = rowA[bi] + (rowA[bi + 1] - rowA[bi]) * bFrac;
    float b = rowB[bi] + (rowB[bi + 1] - rowB[bi]) * bFrac;
    float amp = a + (b - a) * pFrac;
    return amp > 0.05f ? amp : 0.5f + 3.4f * alpha + 0.62f * beta;
}

// pitch in Hz at time fraction u, interpolated in log frequency.
static inline float contourAt(Voice& v, float u) {
    if (v.nContour == 0) return 440.0f;
    if (u <= v.contT[0]) return expf(v.contLog[0]);
    if (u >= v.contT[v.nContour - 1]) return expf(v.contLog[v.nContour - 1]);
    // the cursor only moves forward within a syllable
    while (v.seg < v.nContour - 2 && u > v.contT[v.seg + 1]) v.seg++;
    while (v.seg > 0 && u < v.contT[v.seg]) v.seg--;
    float t0 = v.contT[v.seg], t1 = v.contT[v.seg + 1];
    float frac = (u - t0) / fmaxf(1e-6f, t1 - t0);
    return expf(v.contLog[v.seg] + (v.contLog[v.seg + 1] - v.contLog[v.seg]) * frac);
}

// the §8.1 envelope: linear attack, sustain-or-exponential-decay, 6 ms release.
static float envelopeAt(const Voice& v, int32_t i) {
    int32_t n = v.envN;
    if (n <= 1 || i >= n) return 0;
    float dur = (float)n / s_sr;
    float t = (float)i / s_sr;
    float atk = v.syl->attack > 0 ? v.syl->attack : DEFAULT_ATTACK;
    atk = fminf(fmaxf(atk, 1.0f / s_sr), 0.9f * dur);
    float tau = dur / DECAY_TAU_DIVISOR;

    float env;
    if (t < atk) {
        env = t / atk;
    } else if (v.syl->hold) {
        float knee = HOLD_FRACTION * dur;
        env = t > knee ? expf(-(t - knee) / tau) : 1.0f;
    } else {
        env = expf(-(t - atk) / tau);
    }

    int32_t rel = (int32_t)lroundf(RELEASE_S * s_sr);
    if (rel > 1 && rel < n && i >= n - rel) {
        env *= fmaxf(0.0f, 1.0f - (float)(i - (n - rel)) / (float)(rel - 1));
    }
    return env;
}

// one sample of one oscillator: OVERSAMPLE Euler steps, returns the HP'd source.
static float stepOsc(Voice& v, int index, float alpha, float beta, float amp) {
    float g = v.gamma;
    float g2 = g * g;
    float x = v.x[index];
    float y = v.y[index];
    float dt = v.dt;
    for (int s = 0; s < v.oversample; s++) {
        float dx = y;
        float dy = -alpha * g2 - beta * g2 * x - g2 * x * x * x - g * x * x * y + g2 * x * x - g * x * y;
        x += dx * dt;
        y += dy * dt;
    }
    if (!(fabsf(x) < 1e6f && fabsf(y) < 1e12f)) {
        x = ODE_X0;
        y = 0;
    }
    v.x[index] = x;
    v.y[index] = y;
    // dividing by the peak-to-peak swing keeps loudness tied to the envelope
    float source = (y / g) * SOURCE_GAIN / amp;
    // 180 Hz one-pole highpass ahead of the tract (§8.1)
    float hp = source - v.srcHpX[index] + s_dcCoeff * v.srcHpY[index];
    v.srcHpX[index] = source;
    v.srcHpY[index] = hp;
    return hp;
}

bool syrinxNote(const SpeciesData* species, uint8_t syllableIdx, const SyrinxVoicing& voicing) {
    if (!species || syllableIdx >= species->nSyllables) return false;
    Voice* free = nullptr;
    portENTER_CRITICAL(&s_mux);
    for (auto& v : s_voices) {
        if (!v.active) { free = &v; break; }
    }
    portEXIT_CRITICAL(&s_mux);
    if (!free) return false;

    const SyllableData* syl = &species->syllables[syllableIdx];
    Voice& v = *free;
    v.syl = syl;
    v.dur = syl->dur * voicing.durScale;
    if (v.dur < 0.02f) v.dur = 0.02f;
    v.level = fminf(1.0f, syl->level * voicing.levelScale);
    v.harmonic = fminf(1.0f, syl->harmonic * voicing.harmonicScale);
    v.am = syl->am * voicing.amScale;
    v.amDepth = syl->amDepth;
    v.vibRate = syl->vibRate * voicing.vibScale;
    v.vibDepth = syl->vibDepth;
    v.gain = voicing.gain;

    float logPitch = logf(voicing.pitchMul);
    v.nContour = syl->nContour > MAX_CONTOUR ? MAX_CONTOUR : syl->nContour;
    for (int i = 0; i < v.nContour; i++) {
        v.contT[i] = syl->contour[i].t;
        v.contLog[i] = logf(fmaxf(1.0f, syl->contour[i].hz)) + logPitch;
    }
    v.seg = 0;

    // gamma: pure time-scale, chosen so median pitch sits at beta = 1. The
    // pitch shift scales the contour, so it scales gamma with it.
    v.gamma = fminf(150000.0f, fmaxf(1200.0f, gammaForContour(syl) * voicing.pitchMul));
    v.oversample = oversampleForGamma(v.gamma);
    v.dt = 1.0f / (s_sr * v.oversample);

    v.pos = 0;
    v.durSamples = (int32_t)ceilf((v.dur + 0.02f) * s_sr);
    v.envN = (int32_t)lroundf(v.dur * s_sr);
    if (v.envN < 2) v.envN = 2;

    v.x[0] = v.x[1] = ODE_X0;
    v.y[0] = v.y[1] = 0;
    v.srcHpX[0] = v.srcHpX[1] = v.srcHpY[0] = v.srcHpY[1] = 0;
    v.dcX = v.dcY = v.lpY = 0;
    v.tract[0].reset();
    v.tract[1].reset();
    v.tractHarm[0].reset();
    v.tractHarm[1].reset();
    v.env = 0;

    portENTER_CRITICAL(&s_mux);
    v.active = true;  // last: the render task picks the voice up only fully armed
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void syrinxRender(float* out, int n) {
    memset(out, 0, n * sizeof(float));
    float peak = 0;

    for (int vi = 0; vi < SYRINX_MAX_VOICES; vi++) {
        Voice& v = s_voices[vi];
        if (!v.active) continue;
        const SyllableData* syl = v.syl;
        uint8_t timbre = syl->timbre < 3 ? syl->timbre : 0;
        float tractQ = TIMBRE_Q[timbre];
        float timbreGain = TIMBRE_GAIN[timbre];
        bool twoVoices = syl->two != 0;
        float invDur = 1.0f / v.dur;

        for (int i = 0; i < n; i++) {
            int32_t idx = v.pos;
            if (idx >= v.durSamples) {
                portENTER_CRITICAL(&s_mux);
                v.active = false;
                portEXIT_CRITICAL(&s_mux);
                break;
            }
            v.pos++;
            float t = (float)idx / s_sr;
            float u = fminf(1.0f, t * invDur);

            float f0 = contourAt(v, u);
            if (v.vibRate > 0) f0 *= 1.0f + v.vibDepth * sinf(2.0f * (float)M_PI * v.vibRate * t);

            // AM gates the *pressure* only, never the output gain (§8.1).
            float env = envelopeAt(v, idx);
            float psEnv = env;
            if (v.am > 0) {
                psEnv *= 1.0f - v.amDepth * (0.5f - 0.5f * cosf(2.0f * (float)M_PI * v.am * t));
            }
            float pressure = fminf(1.4f, fmaxf(0.0f, v.level * psEnv * timbreGain));
            v.env = env;
            float alpha = 0.05f + 0.4f * pressure;

            if ((idx & (FILTER_UPDATE - 1)) == 0) {
                v.tract[0].setBandpass(f0, tractQ);
                if (twoVoices) v.tract[1].setBandpass(f0 * DETUNE_TWO, tractQ);
                if (v.harmonic > 0) {
                    v.tractHarm[0].setBandpass(f0 * 2.0f, tractQ);
                    if (twoVoices) v.tractHarm[1].setBandpass(f0 * 2.0f * DETUNE_TWO, tractQ);
                }
            }

            float betaClean = betaForF0(f0, pressure, v.gamma);
            float ampA = ampPP(alpha, betaClean, pressure);
            float source = stepOsc(v, 0, alpha, betaClean, ampA);
            float sample = v.tract[0].process(source);
            if (v.harmonic > 0) sample += v.harmonic * 0.5f * v.tractHarm[0].process(source);

            if (twoVoices) {
                float betaB = betaForF0(f0 * DETUNE_TWO, pressure, v.gamma);
                float ampB = ampPP(alpha, betaB, pressure);
                float sourceB = stepOsc(v, 1, alpha, betaB, ampB);
                float filteredB = v.tract[1].process(sourceB);
                if (v.harmonic > 0) filteredB += v.harmonic * 0.5f * v.tractHarm[1].process(sourceB);
                sample = 0.5f * sample + 0.5f * filteredB;
            }

            // DC block, then tracheal roll-off
            float hp = sample - v.dcX + s_dcCoeff * v.dcY;
            v.dcX = sample;
            v.dcY = hp;
            v.lpY += s_lpCoeff * (hp - v.lpY);

            // the envelope also acts as a linear output gain (§8.1)
            float value = v.lpY * v.gain * env;
            out[i] += value;
            float av = fabsf(value);
            if (av > peak) peak = av;
        }
    }

    // gentle saturation keeps overlapping voices inside [-1, 1]
    for (int i = 0; i < n; i++) {
        out[i] = tanhf(out[i] * 1.6f) * 0.8f;
    }
    s_lastLevel = fmaxf(s_lastLevel * 0.9f, peak);
}

int syrinxActiveVoices() {
    int n = 0;
    for (auto& v : s_voices)
        if (v.active) n++;
    return n;
}

float syrinxLastLevel() { return s_lastLevel; }

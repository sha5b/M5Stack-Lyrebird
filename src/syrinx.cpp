// See syrinx.h. Constants and formulas follow syrinx-processor.js / docs/08 §8.1.
//
// The audible shape is the same as the first port; what changed is where the
// work happens. The old inner loop, per sample per voice, called expf twice
// (contour, envelope), cosf for AM, and then walked two calibration tables:
// a 192-entry binary search in betaForF0 and — the real cost — a linear scan
// in ampPP that starts at index 0 every call and typically runs ~85 iterations
// to reach beta ~= 1. At eight voices that is well over a hundred million table
// steps a second on a 240 MHz core, which overran the I2S DMA. The DAC then got
// handed a late buffer, and a late buffer is a click. That was the crackle.
//
// The tables are regular, which the first port did not exploit:
//   - CAL_INV_F0_GRID is geometric (ratio 1.0150616), so its index is a
//     division in the log domain, and the log of f0 is already being tracked;
//   - CAL_PRESSURE_GRID is uniform (0 .. 1.4 step 0.05);
//   - CAL_BETA_GRID is uniform in three runs (step .01 / .05 / .2).
// So every lookup is now O(1) arithmetic with the same bilinear result, and
// beta and ampPP stay *exact*, per sample. Deep-AM syllables (the wren trill
// modulates pressure 95 % at 28 Hz) depend on that: in the Mindlin model
// pressure moves pitch, so approximating beta at control rate audibly flattens
// them.
//
// What did move to a 32-sample control block is f0 itself — the contour, the
// tract retune and vibrato — with pitch tracked between blocks by a
// multiplicative ramp, so nothing steps. The tract was already retuned at that
// rate in the first port.
//
// Also incremental now, and exact: the envelope (linear attack, recursive
// exponential decay, linear release) and the AM oscillator (unit-circle
// rotation) — no libm in the sample loop at all.
#include "syrinx.h"
#include "calibration.h"
#include <math.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#define MAX_CONTOUR 8
#define CTRL_MASK (SYRINX_CTRL - 1)

static const float CALIBRATION_GAMMA = 24000.0f; // gamma the offline table was measured at
static const float PHI_AT_BETA_ONE = 0.17061f;   // measured f0/gamma at alpha 0.1, beta 1
static const float DETUNE_TWO = 1.0040348f;      // +7 cents: the second syringeal side
static const float LOG_DETUNE_TWO = 0.0040267f;  // logf(DETUNE_TWO)
static const float DEFAULT_ATTACK = 0.008f;
static const float RELEASE_S = 0.006f;
static const float DECAY_TAU_DIVISOR = 2.5f;
static const float HOLD_FRACTION = 0.75f;
static const float SOURCE_GAIN = 0.35f;
static const float ODE_X0 = 1.0e-4f;

// Mix soft-clip: tanh(drive * x) * trim, so small-signal gain is drive * trim
// and the ceiling is trim. The first port used 1.6 / 0.8 — same gain, but a
// ceiling of 0.8 puts a duet into hard saturation, and that grit reads as
// crackle on an 8-bit DAC. Keeping the gain (1.216 vs 1.28, half a dB) while
// lifting the ceiling moves the knee above where a chorus sits.
static const float MIX_DRIVE = 1.28f;
static const float MIX_TRIM = 0.95f;

// timbre classes: {gain, q}
static const float TIMBRE_GAIN[3] = {1.0f, 1.25f, 1.75f};
static const float TIMBRE_Q[3] = {9.0f, 5.0f, 3.0f};

// uniform-grid constants, asserted against the tables in syrinxInit
static const float PRESSURE_STEP_INV = 20.0f;   // 1 / 0.05
static const int BETA_SEG1 = 85;                // beta 0.15 .. 1.00, step 0.01
static const int BETA_SEG2 = 145;               // beta 1.00 .. 4.00, step 0.05
                                                // beta 4.00 .. 16.0, step 0.20

static float s_sr = 22050.0f;
static float s_invSr = 1.0f / 22050.0f;
static float s_dcCoeff;  // 180 Hz one-pole (source HP and output DC block)
static float s_lpCoeff;  // 12 kHz one-pole lowpass
static float s_f0LogBase;  // logf(CAL_INV_F0_GRID[0])
static float s_f0InvLogR;  // 1 / logf(grid ratio)

// tanh to ~1e-5 on [-3, 3] (order 7/6 Pade), flat outside. The low-order form
// is 2 % off at the knee, which is a different clipper, not this one.
static inline float fastTanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    float x2 = x * x;
    float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2));
    return num / den;
}

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
    float amDepth = 0;
    float gain = 1;
    uint8_t tag = 0;
    // contour cache: log(hz * pitchMul) per point, plus segment cursor
    uint8_t nContour = 0;
    float contT[MAX_CONTOUR];
    float contLog[MAX_CONTOUR];
    uint8_t seg = 0;

    int32_t pos = 0;       // samples since onset
    int32_t durSamples = 0;
    int32_t envN = 0;

    // pitch: set at each control block, ramped per sample. Both the linear and
    // the log value are carried — the grid index needs the log, the in-cell
    // fraction needs the frequency.
    float f0 = 440.0f, logF0 = 6.0866f;
    float f0Ratio = 1.0f, logF0Inc = 0.0f;
    float gammaK = 1.0f, logGammaK = 0.0f;   // CALIBRATION_GAMMA / gamma

    // envelope, evaluated incrementally; envPhase is just "decay has started"
    uint8_t envPhase = 0;
    bool envHold = false;
    int32_t envAtkN = 1, envKneeN = 0, envRelStart = 0, envRelN = 0;
    float envAtkInc = 1, envAtkS = 0, envKneeS = 0, envInvTau = 0;
    float envDecMul = 1, envRelInc = 0;
    float env = 0;

    // recursive quadrature oscillators (rotate one sample / one control block)
    bool hasAm = false, hasVib = false;
    float amC = 1, amS = 0, amRotC = 1, amRotS = 0;
    float vibC = 1, vibS = 0, vibRotC = 1, vibRotS = 0, vibDepth = 0;

    float gamma = CALIBRATION_GAMMA;
    int oversample = 16;
    float dt = 1.0f / (22050.0f * 16.0f);
    float x[2] = {ODE_X0, ODE_X0};
    float y[2] = {0, 0};
    Biquad tract[2];
    Biquad tractHarm[2];
    float srcHpX[2] = {0, 0}, srcHpY[2] = {0, 0};
    float dcX = 0, dcY = 0, lpY = 0;
};

static Voice s_voices[SYRINX_MAX_VOICES];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_lastLevel = 0;

void syrinxInit(float sampleRate) {
    s_sr = sampleRate;
    s_invSr = 1.0f / sampleRate;
    s_dcCoeff = expf(-2.0f * (float)M_PI * 180.0f / s_sr);
    s_lpCoeff = 1.0f - expf(-2.0f * (float)M_PI * 12000.0f / s_sr);
    s_f0LogBase = logf(CAL_INV_F0_GRID[0]);
    s_f0InvLogR = 1.0f / logf(CAL_INV_F0_GRID[1] / CAL_INV_F0_GRID[0]);
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

// Row index + in-row fraction on the uniform pressure grid, O(1). Equivalent to
// the old linear scan, which stopped at the last row whose successor is not
// below `pressure`, then clamped the fraction.
static inline int pressureRow(float pressure, float& frac) {
    float p = pressure * PRESSURE_STEP_INV;
    int pi = (int)p;
    if (pi < 0) pi = 0;
    else if (pi > CAL_PRESSURE_GRID_LEN - 2) pi = CAL_PRESSURE_GRID_LEN - 2;
    float f = p - (float)pi;
    frac = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    return pi;
}

// beta for a target f0 via the offline table (bilinear over pressure x f0).
// logF0 is log(targetF0); the caller already tracks it, and the f0 grid is
// geometric, so the column index is one multiply instead of a binary search.
static inline float betaForF0(float targetF0, float logF0, float pressure, float gammaK,
                              float logGammaK) {
    float f0 = targetF0 * gammaK;
    float pFrac;
    int pi = pressureRow(pressure, pFrac);

    float clamped = fminf(fmaxf(f0, CAL_INV_F0_GRID[0]), CAL_INV_F0_GRID[CAL_INV_F0_GRID_LEN - 1]);
    int lo = (int)((logF0 + logGammaK - s_f0LogBase) * s_f0InvLogR);
    if (lo < 0) lo = 0;
    else if (lo > CAL_INV_F0_GRID_LEN - 2) lo = CAL_INV_F0_GRID_LEN - 2;
    // the grid is geometric to 2.5e-6, not exactly, so nudge onto the true cell
    if (lo < CAL_INV_F0_GRID_LEN - 2 && CAL_INV_F0_GRID[lo + 1] <= clamped) lo++;
    else if (lo > 0 && CAL_INV_F0_GRID[lo] > clamped) lo--;
    int hi = lo + 1;

    float fSpan = CAL_INV_F0_GRID[hi] - CAL_INV_F0_GRID[lo];
    if (fSpan == 0) fSpan = 1;
    float fFrac = (clamped - CAL_INV_F0_GRID[lo]) / fSpan;

    const float* rowA = CAL_INV_BETA[pi];
    const float* rowB = CAL_INV_BETA[pi + 1];
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
// follows the envelope instead of the pitch (§8.1). The beta grid is uniform
// in three runs, so its index is arithmetic rather than a 205-step scan.
static inline float ampPP(float alpha, float beta, float pressure) {
    float pFrac;
    int pi = pressureRow(pressure, pFrac);

    int bi;
    if (beta < 1.0f) bi = (int)((beta - 0.15f) * 100.0f);
    else if (beta < 4.0f) bi = BETA_SEG1 + (int)((beta - 1.0f) * 20.0f);
    else bi = BETA_SEG2 + (int)((beta - 4.0f) * 5.0f);
    if (bi < 0) bi = 0;
    else if (bi > CAL_BETA_GRID_LEN - 2) bi = CAL_BETA_GRID_LEN - 2;

    float bSpan = CAL_BETA_GRID[bi + 1] - CAL_BETA_GRID[bi];
    if (bSpan == 0) bSpan = 1;
    float bFrac = (beta - CAL_BETA_GRID[bi]) / bSpan;
    if (bFrac < 0.0f) bFrac = 0.0f;
    else if (bFrac > 1.0f) bFrac = 1.0f;

    const float* rowA = CAL_AMP_PP[pi];
    const float* rowB = CAL_AMP_PP[pi + 1];
    float a = rowA[bi] + (rowA[bi + 1] - rowA[bi]) * bFrac;
    float b = rowB[bi] + (rowB[bi + 1] - rowB[bi]) * bFrac;
    float amp = a + (b - a) * pFrac;
    return amp > 0.05f ? amp : 0.5f + 3.4f * alpha + 0.62f * beta;
}

// log(pitch in Hz) at time fraction u — the contour is stored in log already,
// so the control block pays one expf instead of the old one per sample.
static inline float contourLogAt(Voice& v, float u) {
    if (v.nContour == 0) return 6.0866f;  // log(440)
    if (u <= v.contT[0]) return v.contLog[0];
    if (u >= v.contT[v.nContour - 1]) return v.contLog[v.nContour - 1];
    // the cursor only moves forward within a syllable
    while (v.seg < v.nContour - 2 && u > v.contT[v.seg + 1]) v.seg++;
    while (v.seg > 0 && u < v.contT[v.seg]) v.seg--;
    float t0 = v.contT[v.seg], t1 = v.contT[v.seg + 1];
    float frac = (u - t0) / fmaxf(1e-6f, t1 - t0);
    return v.contLog[v.seg] + (v.contLog[v.seg + 1] - v.contLog[v.seg]) * frac;
}

// one sample of one oscillator: OVERSAMPLE Euler steps, returns the HP'd source.
static inline float stepOsc(Voice& v, int index, float alpha, float beta, float amp) {
    float g = v.gamma;
    float g2 = g * g;
    float x = v.x[index];
    float y = v.y[index];
    float dt = v.dt;
    float aG2 = alpha * g2;
    float bG2 = beta * g2;
    for (int s = 0; s < v.oversample; s++) {
        float x2 = x * x;
        float dy = -aG2 - bG2 * x - g2 * x2 * x - g * x2 * y + g2 * x2 - g * x * y;
        x += y * dt;
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
    v.amDepth = syl->amDepth;
    v.gain = voicing.gain;
    v.tag = voicing.tag;

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
    v.gammaK = CALIBRATION_GAMMA / v.gamma;
    v.logGammaK = logf(v.gammaK);

    v.pos = 0;
    v.durSamples = (int32_t)ceilf((v.dur + 0.02f) * s_sr);
    v.envN = (int32_t)lroundf(v.dur * s_sr);
    if (v.envN < 2) v.envN = 2;

    // Envelope as increments. The thresholds are the exact sample at which the
    // old closed form changed branch: ceil for the attack (which tested t < atk)
    // and floor+1 for the hold knee (which tested t > knee), so a note that does
    // not land on a sample boundary still lines up.
    float envDur = (float)v.envN * s_invSr;
    float atk = syl->attack > 0 ? syl->attack : DEFAULT_ATTACK;
    atk = fminf(fmaxf(atk, s_invSr), 0.9f * envDur);
    v.envAtkS = atk;
    v.envAtkN = (int32_t)ceilf(atk * s_sr);
    if (v.envAtkN < 1) v.envAtkN = 1;
    v.envAtkInc = 1.0f / (atk * s_sr);
    v.envHold = syl->hold != 0;
    v.envKneeS = HOLD_FRACTION * envDur;
    v.envKneeN = (int32_t)floorf(v.envKneeS * s_sr) + 1;
    float tau = envDur / DECAY_TAU_DIVISOR;
    v.envInvTau = 1.0f / fmaxf(1e-4f, tau);
    v.envDecMul = expf(-s_invSr * v.envInvTau);
    int32_t rel = (int32_t)lroundf(RELEASE_S * s_sr);
    v.envRelN = (rel > 1 && rel < v.envN) ? rel : 0;
    v.envRelStart = v.envN - v.envRelN;
    v.envRelInc = v.envRelN > 1 ? 1.0f / (float)(v.envRelN - 1) : 0.0f;
    v.envPhase = 0;
    v.env = 0;

    // AM / vibrato as unit-circle rotations: AM steps a sample at a time,
    // vibrato a control block at a time (one authored syllable uses it).
    float am = syl->am * voicing.amScale;
    v.hasAm = am > 0.0f && syl->amDepth > 0.0f;
    if (v.hasAm) {
        float w = 2.0f * (float)M_PI * am * s_invSr;
        v.amRotC = cosf(w);
        v.amRotS = sinf(w);
    }
    v.amC = 1.0f;  // cos(0): the AM factor starts un-gated, as at t = 0
    v.amS = 0.0f;

    float vibRate = syl->vibRate * voicing.vibScale;
    v.hasVib = vibRate > 0.0f && syl->vibDepth > 0.0f;
    v.vibDepth = syl->vibDepth;
    if (v.hasVib) {
        float w = 2.0f * (float)M_PI * vibRate * SYRINX_CTRL * s_invSr;
        v.vibRotC = cosf(w);
        v.vibRotS = sinf(w);
    }
    v.vibC = 1.0f;  // sin(0) = 0: no pitch offset on the first block
    v.vibS = 0.0f;

    v.x[0] = v.x[1] = ODE_X0;
    v.y[0] = v.y[1] = 0;
    v.srcHpX[0] = v.srcHpX[1] = v.srcHpY[0] = v.srcHpY[1] = 0;
    v.dcX = v.dcY = v.lpY = 0;
    v.tract[0].reset();
    v.tract[1].reset();
    v.tractHarm[0].reset();
    v.tractHarm[1].reset();

    v.logF0 = contourLogAt(v, 0.0f);
    v.f0 = expf(v.logF0);
    v.f0Ratio = 1.0f;
    v.logF0Inc = 0.0f;

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
        const float tractQ = TIMBRE_Q[timbre];
        const float timbreGain = TIMBRE_GAIN[timbre];
        const bool twoVoices = syl->two != 0;
        const bool harm = v.harmonic > 0;
        const float harmGain = v.harmonic * 0.5f;
        const float invDur = 1.0f / v.dur;

        int i = 0;
        while (i < n) {
            int32_t idx = v.pos;
            if (idx >= v.durSamples) {
                portENTER_CRITICAL(&s_mux);
                v.active = false;
                portEXIT_CRITICAL(&s_mux);
                break;
            }

            // ---- control rate: the pitch contour and the tract tuning -------
            if ((idx & CTRL_MASK) == 0) {
                float uNow = fminf(1.0f, (float)idx * s_invSr * invDur);
                float uEnd = fminf(1.0f, (float)(idx + SYRINX_CTRL) * s_invSr * invDur);
                float logNow = contourLogAt(v, uNow);
                float logEnd = contourLogAt(v, uEnd);
                if (v.hasVib) {
                    // vibrato lives in the log chain so f0 and logF0 stay in step
                    logNow += log1pf(v.vibDepth * v.vibS);
                    float nc = v.vibC * v.vibRotC - v.vibS * v.vibRotS;
                    float ns = v.vibS * v.vibRotC + v.vibC * v.vibRotS;
                    v.vibC = nc;
                    v.vibS = ns;
                    logEnd += log1pf(v.vibDepth * v.vibS);
                }
                v.logF0 = logNow;
                v.f0 = expf(logNow);  // re-anchored each block: the ramp cannot drift
                v.logF0Inc = (logEnd - logNow) * (1.0f / (float)SYRINX_CTRL);
                v.f0Ratio = expf(v.logF0Inc);

                v.tract[0].setBandpass(v.f0, tractQ);
                if (harm) v.tractHarm[0].setBandpass(v.f0 * 2.0f, tractQ);
                if (twoVoices) {
                    v.tract[1].setBandpass(v.f0 * DETUNE_TWO, tractQ);
                    if (harm) v.tractHarm[1].setBandpass(v.f0 * 2.0f * DETUNE_TWO, tractQ);
                }
            }

            // run to the end of this control block, this render buffer, or the note
            int chunk = SYRINX_CTRL - (int)(idx & CTRL_MASK);
            if (chunk > n - i) chunk = n - i;
            if (idx + chunk > v.durSamples) chunk = (int)(v.durSamples - idx);

            for (int k = 0; k < chunk; k++, i++, idx++) {
                // ---- envelope, incrementally -------------------------------
                float env;
                if (idx >= v.envN) {
                    env = 0.0f;
                    v.env = 0.0f;
                } else {
                    if (idx < v.envAtkN) {
                        v.env = (float)idx * v.envAtkInc;
                    } else if (v.envHold && idx < v.envKneeN) {
                        v.env = 1.0f;
                    } else if (v.envPhase) {
                        v.env *= v.envDecMul;
                    } else {
                        // first decay sample: land exactly on the closed form,
                        // then recurse. One expf per note, none per sample.
                        v.envPhase = 1;
                        float t = (float)idx * s_invSr;
                        float ref = v.envHold ? v.envKneeS : v.envAtkS;
                        v.env = t > ref ? expf(-(t - ref) * v.envInvTau) : 1.0f;
                    }
                    env = v.env;
                    if (v.envRelN && idx >= v.envRelStart) {
                        float r = 1.0f - (float)(idx - v.envRelStart) * v.envRelInc;
                        env *= r > 0.0f ? r : 0.0f;
                    }
                }

                // AM gates the *pressure* only, never the output gain (§8.1).
                float psEnv = env;
                if (v.hasAm) {
                    psEnv *= 1.0f - v.amDepth * (0.5f - 0.5f * v.amC);
                    float nc = v.amC * v.amRotC - v.amS * v.amRotS;
                    float ns = v.amS * v.amRotC + v.amC * v.amRotS;
                    v.amC = nc;
                    v.amS = ns;
                }
                float pressure = v.level * psEnv * timbreGain;
                if (pressure > 1.4f) pressure = 1.4f;
                else if (pressure < 0.0f) pressure = 0.0f;
                float alpha = 0.05f + 0.4f * pressure;

                float beta = betaForF0(v.f0, v.logF0, pressure, v.gammaK, v.logGammaK);
                float amp = ampPP(alpha, beta, pressure);
                float source = stepOsc(v, 0, alpha, beta, amp);
                float sample = v.tract[0].process(source);
                if (harm) sample += harmGain * v.tractHarm[0].process(source);

                if (twoVoices) {
                    float betaB = betaForF0(v.f0 * DETUNE_TWO, v.logF0 + LOG_DETUNE_TWO,
                                            pressure, v.gammaK, v.logGammaK);
                    float ampB = ampPP(alpha, betaB, pressure);
                    float sourceB = stepOsc(v, 1, alpha, betaB, ampB);
                    float filteredB = v.tract[1].process(sourceB);
                    if (harm) filteredB += harmGain * v.tractHarm[1].process(sourceB);
                    sample = 0.5f * sample + 0.5f * filteredB;
                }

                // advance the pitch ramp for the next sample
                v.f0 *= v.f0Ratio;
                v.logF0 += v.logF0Inc;

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
            v.pos = idx;
        }
    }

    // gentle saturation keeps overlapping voices inside [-1, 1]
    for (int i = 0; i < n; i++) {
        out[i] = fastTanh(out[i] * MIX_DRIVE) * MIX_TRIM;
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

int syrinxSnapshot(SyrinxVoiceInfo* out, int max) {
    int n = 0;
    for (auto& v : s_voices) {
        if (n >= max) break;
        if (!v.active) continue;
        out[n].f0 = v.f0;
        out[n].env = v.env;
        out[n].tag = v.tag;
        n++;
    }
    return n;
}

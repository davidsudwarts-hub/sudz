#pragma once
/*  Sudz live effects: the vocal polish chain running in real time inside GarageBand.

    It follows the same settings as the polish chain in the editor (the "pol" settings: low cut, rider,
    gate, compressor, saturation, tone, exciter, de-esser, reverb, doubler, chorus, slapback, echo,
    output level), so a style chosen in the editor sounds the same way live. The offline chain in the
    editor can look at the whole song (tone matching, noise reduction, de-reverb, look-ahead rider);
    this one can only look backwards, so those parts are left to the editor's export.

    Plain C++17, no JUCE: allocation happens in prepare() only, process() is real-time safe.
*/
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sudzfx
{
constexpr double kPi = 3.14159265358979323846;
inline float dbToG (float d) { return std::pow (10.0f, d / 20.0f); }
inline float gToDb (float g) { return 20.0f * std::log10 (g + 1e-12f); }

//==============================================================================
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Type { HP, LP, Peak, LowShelf, HighShelf };

    void set (Type t, double f0, double sr, double q = 0.7071, double gainDb = 0)
    {
        const double w = 2 * kPi * std::min (f0, sr * 0.49) / sr, c = std::cos (w), al = std::sin (w) / (2 * q), A = std::pow (10.0, gainDb / 40);
        double B0, B1, B2, A0, A1, A2;
        switch (t)
        {
            case HP:   B0 = (1 + c) / 2; B1 = -(1 + c); B2 = B0; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
            case LP:   B0 = (1 - c) / 2; B1 = 1 - c; B2 = B0; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
            case Peak: B0 = 1 + al * A; B1 = -2 * c; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * c; A2 = 1 - al / A; break;
            case LowShelf:
            case HighShelf:
            default:
            {
                const double sq = 2 * std::sqrt (A) * al, sg = t == HighShelf ? 1 : -1;
                B0 = A * ((A + 1) + sg * (A - 1) * c + sq); B1 = -2 * sg * A * ((A - 1) + sg * (A + 1) * c); B2 = A * ((A + 1) + sg * (A - 1) * c - sq);
                A0 = (A + 1) - sg * (A - 1) * c + sq; A1 = 2 * sg * ((A - 1) - sg * (A + 1) * c); A2 = (A + 1) - sg * (A - 1) * c - sq;
            }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    inline float process (float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
    void reset() { z1 = z2 = 0; }
};

// circular delay line with fractional read
struct Delay
{
    std::vector<float> buf; int w = 0, len = 1;
    void prepare (int maxSamples) { len = std::max (4, maxSamples); buf.assign ((size_t) len, 0.0f); w = 0; }
    inline void push (float x) { buf[(size_t) w] = x; if (++w == len) w = 0; }
    inline float read (double d) const   // d samples ago (1 = the sample just pushed)
    {
        d = std::min (std::max (d, 1.0), (double) len - 2);
        double p = w - d; while (p < 0) p += len;
        const int i0 = (int) p; const float f = (float) (p - i0); const int i1 = i0 + 1 == len ? 0 : i0 + 1;
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * f;
    }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
};

//==============================================================================
struct Settings
{
    // the editor's polish keys (same names and units)
    float lowcut = 100, gate = 0, even = 1, punch = 0, warmth = 0, body = 0, mud = 0, presence = 0, air = 0,
          exciter = 0, deess = 6, deessHz = 6900, verb = 0.6f, size = 1.6f, dbl = 0, chorus = 0, slap = 0, echo = 0, echoBeats = 0.5f, level = -18;
    float vocalDb = NAN;       // the take's sung level as the editor measured it (seeds the live level trackers)
    // the editor's tone matching (reference vocal or the style's default) as 1/3-octave peaking gains in dB
    std::array<float, 25> toneEq {}; bool hasToneEq = false;
    static constexpr float toneFreqs[25] = { 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000 };
    int verbType = 2;          // 0 room, 1 chamber, 2 plate, 3 hall
    bool on = true;

    /** Sets one value by its editor name; returns false for names this chain doesn't use. */
    bool set (const std::string& k, double v)
    {
        if (! std::isfinite (v)) return false;
        const float f = (float) v;
        if (k == "lowcut") lowcut = std::clamp (f, 20.0f, 400.0f);
        else if (k == "gate") gate = std::clamp (f, 0.0f, 1.0f);
        else if (k == "even") even = std::clamp (f, 0.0f, 2.0f);
        else if (k == "punch") punch = std::clamp (f, 0.0f, 1.0f);
        else if (k == "warmth") warmth = std::clamp (f, 0.0f, 1.0f);
        else if (k == "body") body = std::clamp (f, -12.0f, 12.0f);
        else if (k == "mud") mud = std::clamp (f, -12.0f, 12.0f);
        else if (k == "presence") presence = std::clamp (f, -12.0f, 12.0f);
        else if (k == "air") air = std::clamp (f, -12.0f, 12.0f);
        else if (k == "exciter") exciter = std::clamp (f, 0.0f, 1.0f);
        else if (k == "deess") deess = std::clamp (f, 0.0f, 24.0f);
        else if (k == "deessHz") deessHz = f > 0 ? std::clamp (f, 3000.0f, 12000.0f) : 6900.0f;
        else if (k == "verb") verb = std::clamp (f, 0.0f, 1.0f);
        else if (k == "size") size = std::clamp (f, 0.3f, 5.0f);
        else if (k == "double") dbl = std::clamp (f, 0.0f, 1.0f);
        else if (k == "chorus") chorus = std::clamp (f, 0.0f, 1.0f);
        else if (k == "slap") slap = std::clamp (f, 0.0f, 1.0f);
        else if (k == "echo") echo = std::clamp (f, 0.0f, 1.0f);
        else if (k == "echoBeats") echoBeats = std::clamp (f, 0.0625f, 2.0f);
        else if (k == "level") level = std::clamp (f, -30.0f, -6.0f);
        else if (k == "vocalDb") vocalDb = std::clamp (f, -70.0f, 0.0f);
        else return false;
        return true;
    }
    bool setList (const std::string& k, const std::vector<double>& v)
    {
        if (k != "toneEq" || v.size() != toneEq.size()) return false;
        hasToneEq = false;
        for (size_t i = 0; i < v.size(); ++i) { toneEq[i] = std::isfinite (v[i]) ? std::clamp ((float) v[i], -18.0f, 15.0f) : 0.0f; if (std::abs (toneEq[i]) > 0.05f) hasToneEq = true; }
        return true;
    }
    bool setString (const std::string& k, const std::string& v)
    {
        if (k != "verbType") return false;
        verbType = v == "room" ? 0 : v == "chamber" ? 1 : v == "hall" ? 3 : 2;
        return true;
    }
};

/** Host-automatable rides on top of the editor's settings (GarageBand automation lanes). */
struct Rides
{
    float reverb = 1, echo = 1, dbl = 1, outDb = 0;   // multipliers 0..2, output trim in dB
};

//==============================================================================
class Chain
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const double k = sr / 44100.0;
        for (auto& d : fdn) d.prepare ((int) (3109 * k * 1.35 * (0.6 + 0.4 * 5.0 / 1.6)) + 8);
        preDelay.prepare ((int) (0.05 * sr) + 8);
        dblL.prepare ((int) (0.03 * sr)); dblR.prepare ((int) (0.03 * sr));
        cho.prepare ((int) (0.04 * sr));
        slapD.prepare ((int) (0.12 * sr));
        echoL.prepare ((int) (4.2 * sr)); echoR.prepare ((int) (4.2 * sr));
        reset();
        cur = Settings{}; dirty = true;
    }

    void reset()
    {
        for (auto* b : { &hp1, &hp2, &hp3, &eqBody, &eqMud, &eqPres, &eqAir, &dsH1, &dsH2, &exH1, &exH2, &exH3, &exH4,
                         &vSendHP, &vSendLP, &sSendHP, &sSendLP, &eSendHP, &eSendLP })
            b->reset();
        for (auto& d : fdn) d.clear();
        for (auto* d : { &preDelay, &dblL, &dblR, &cho, &slapD, &echoL, &echoR }) d->clear();
        std::fill (std::begin (fdnLp), std::end (fdnLp), 0.0);
        rmsFast = rmsSlow = 0; riderDb = 0; sungDb = -24; sungSeen = 0; compEnv = 0; compGr = 0; punchEnv = 0; punchGr = 0;
        gateEnv = 0; gateDb = 0; outSung = -24; outE = 0; outSeen = 0; gateHold = 0; dsEh = dsEf = 0; dsG = 0; duckV = duckE = 0; outDb = 6; limG = 1;
        phase = 0; echoLpL = echoLpR = 0;
    }

    /** Message thread: hand over new settings (picked up at the start of the next block). */
    void setSettings (const Settings& s)
    {
        std::lock_guard<std::mutex> g (pendingLock);
        pending = s; havePending = true;
    }

    /** Audio thread. L and R are processed in place (R may equal L for a mono track: pass the same pointer). */
    void process (float* L, float* R, int n, double bpm, const Rides& rides)
    {
        if (pendingLock.try_lock())
        {
            if (havePending)
            {
                if (std::isfinite (pending.vocalDb) && ! (std::abs (pending.vocalDb - cur.vocalDb) < 0.5f)) { sungDb = pending.vocalDb; sungSeen = 2 * sr; }
                cur = pending; havePending = false; dirty = true;
            }
            pendingLock.unlock();
        }
        if (dirty) { update(); dirty = false; }
        if (! cur.on) return;

        const bool stereoOut = R != L;
        const float verbAmt = std::clamp (cur.verb * rides.reverb, 0.0f, 1.0f), echoAmt = std::clamp (cur.echo * rides.echo, 0.0f, 1.0f),
                    dblAmt = std::clamp (cur.dbl * rides.dbl, 0.0f, 1.0f);
        const double echoSamples = std::clamp (sr * 60.0 / std::max (40.0, bpm > 0 ? bpm : 120.0) * cur.echoBeats, 1.0, 4.0 * sr);

        for (int i = 0; i < n; ++i)
        {
            float x = stereoOut ? 0.5f * (L[i] + R[i]) : L[i];

            // ---- low cut (three stages, like the editor)
            x = hp3.process (hp2.process (hp1.process (x)));

            // ---- level tracking: the vocal's sung level (slow) and the 300 ms level (rider)
            rmsFast = aFast * rmsFast + (1 - aFast) * x * x + 1e-20;
            const float lvl = 10.0f * std::log10 ((float) rmsFast + 1e-14f);
            if (lvl > sungDb - 18.0f && lvl > -60.0f)
            {
                const double a = sungSeen < 2.0 * sr ? aSungQuick : aSung;
                sungDb = (float) (a * sungDb + (1 - a) * lvl); sungSeen += 1;
            }
            // rider: inside phrases bring the level toward the sung level (boost at most 6 dB); in gaps never boost
            {
                const float want = lvl > sungDb - 20.0f ? std::clamp (sungDb - lvl, -10.0f, 6.0f) * std::min (1.2f, 0.85f * cur.even)
                                                        : std::min (0.0f, riderDb);
                riderDb = (float) (aRide * riderDb + (1 - aRide) * want);
                x *= dbToG (riderDb);
            }

            // ---- noise gate (between phrases only)
            if (cur.gate > 0)
            {
                gateEnv = aGateE * gateEnv + (1 - aGateE) * x * x + 1e-20;
                const float gl = 10.0f * std::log10 ((float) gateEnv), thr = sungDb - 22.0f;
                float want = gl < thr ? -std::min (6.0f + 24.0f * cur.gate, (thr - gl) * 2.0f) : 0.0f;
                if (want >= 0.0f) gateHold = (int) (0.12 * sr); else if (gateHold > 0) { --gateHold; want = 0.0f; }
                gateDb = (float) (want > gateDb ? aOpen * gateDb + (1 - aOpen) * want : aClose * gateDb + (1 - aClose) * want);
                x *= dbToG (gateDb);
            }

            // ---- compressor (soft knee) and parallel "punch" compression
            if (cur.even > 0)
            {
                compEnv = aDet * compEnv + (1 - aDet) * x * x + 1e-20;
                const float over = 10.0f * std::log10 ((float) compEnv) - (sungDb - 6.0f), ratio = 1.0f + 1.5f * cur.even, knee = 6.0f;
                const float want = over <= -knee / 2 ? 0.0f : over >= knee / 2 ? over * (1 - 1 / ratio) : (1 - 1 / ratio) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
                compGr = (float) (want > compGr ? aAtt * compGr + (1 - aAtt) * want : aRel * compGr + (1 - aRel) * want);
                x *= dbToG (-compGr);
            }
            if (cur.punch > 0)
            {
                punchEnv = aDet * punchEnv + (1 - aDet) * x * x + 1e-20;
                const float over = 10.0f * std::log10 ((float) punchEnv) - (sungDb - 20.0f);
                const float want = std::max (0.0f, over * 0.9f);
                punchGr = (float) (want > punchGr ? aPunchA * punchGr + (1 - aPunchA) * want : aPunchR * punchGr + (1 - aPunchR) * want);
                x += cur.punch * 0.7f * x * dbToG (std::min (18.0f, 12.0f - punchGr));
            }

            // ---- saturation (warmth): soft tanh, scaled to the voice's level
            if (cur.warmth > 0)
            {
                const float k = 0.18f / std::max (1e-6f, dbToG (sungDb)), drive = 1 + 5 * cur.warmth, v = x * k;
                x = ((1 - cur.warmth) * v + cur.warmth * std::tanh (drive * v) / std::tanh (drive)) / k;
            }

            // ---- tone
            if (std::abs (cur.body) > 1e-6f) x = eqBody.process (x);
            if (std::abs (cur.mud) > 1e-6f) x = eqMud.process (x);
            if (std::abs (cur.presence) > 1e-6f) x = eqPres.process (x);
            if (std::abs (cur.air) > 1e-6f) x = eqAir.process (x);
            if (cur.hasToneEq) for (size_t k = 0; k < eqTone.size(); ++k) if (toneOn[k]) x = eqTone[k].process (x);

            // ---- exciter: harmonics generated from the top end
            if (cur.exciter > 0)
            {
                const float k = 4.0f / std::max (1e-6f, dbToG (sungDb - 12.0f));
                const float h = exH2.process (exH1.process (x));
                const float e = exH4.process (exH3.process (std::tanh (h * k) / k));
                x += 0.9f * cur.exciter * e;
            }

            // ---- de-esser: turns the hiss band down when it dominates
            if (cur.deess > 0)
            {
                const float hf = dsH2.process (dsH1.process (x));
                dsEh = aDs * dsEh + (1 - aDs) * hf * hf + 1e-24; dsEf = aDs * dsEf + (1 - aDs) * x * x + 1e-24;
                const float ratio = 10.0f * std::log10 ((float) ((dsEh + 1e-14) / (dsEf + 1e-14)));
                const float want = std::clamp ((ratio + 6.0f) * 1.5f, 0.0f, cur.deess);
                dsG = (float) (want > dsG ? want : aDsR * dsG + (1 - aDsR) * want);
                x -= hf * (1.0f - dbToG (-dsG));
            }

            // ======== stereo: reverb, doubler, chorus, slapback, echo
            float outL = x, outR = x;

            // ducking reference: the voice's level (reverb and echo sit lower while singing, bloom in the gaps)
            const float x2 = x * x;
            duckV = (x2 > duckV ? aDuckA * duckV + (1 - aDuckA) * x2 : aDuckRv * duckV + (1 - aDuckRv) * x2) + 1e-24;
            duckE = (x2 > duckE ? aDuckA * duckE + (1 - aDuckA) * x2 : aDuckRe * duckE + (1 - aDuckRe) * x2) + 1e-24;
            const float duckRef = sungDb + riderDb;   // the level after the rider

            if (verbAmt > 0)
            {
                preDelay.push (vSendLP.process (vSendHP.process (x)));
                const float inp = preDelay.read (std::max (1.0, preSamples)) * 0.35f;
                float sum = 0, l = 0, r = 0, outs[8];
                const double dn = (i & 1) ? 1e-20 : -1e-20;
                for (size_t k = 0; k < 8; ++k) { const float v = fdn[k].read (fdnLen[k]); fdnLp[k] = fdnLp[k] * damp + v * (1 - damp) + dn; outs[k] = (float) fdnLp[k]; sum += outs[k]; }
                const float hh = sum * 0.25f;
                for (size_t k = 0; k < 8; ++k) { fdn[k].push ((outs[k] - hh) * fdnFb[k] + inp * TI[k]); l += outs[k] * TL[k]; r += outs[k] * TR[k]; }
                const float dk = std::clamp ((10.0f * std::log10 ((float) duckV) - (duckRef - 14.0f)) / 10.0f, 0.0f, 1.0f);
                const float wg = verbAmt * 0.5f * dbToG (-6.0f * dk);
                outL += wg * l; outR += wg * r;
            }
            else { for (auto& d : fdn) d.push (0.0f); preDelay.push (0.0f); }

            phase += 1.0 / sr;
            if (phase > 1000.0) phase -= 1000.0;   // keeps the LFOs precise over long sessions (all rates divide 1000 s)
            dblL.push (x); dblR.push (x); cho.push (x);
            if (dblAmt > 0)
            {
                const double p1 = 0.011 * sr + 0.0004 * sr * std::sin (2 * kPi * 0.31 * phase), p2 = 0.017 * sr + 0.0005 * sr * std::sin (2 * kPi * 0.23 * phase + 1.3);
                outL += 0.42f * dblAmt * dblL.read (p1); outR += 0.42f * dblAmt * dblR.read (p2);
            }
            if (cur.chorus > 0)
            {
                static constexpr double V[3][5] = { { 0.019, 0.0028, 0.83, 0, 1 }, { 0.025, 0.0032, 1.07, 1.9, -1 }, { 0.022, 0.0025, 0.61, 3.7, 0 } };
                const float g = 0.38f * cur.chorus;
                for (auto& v : V)
                {
                    const float s = cho.read (v[0] * sr + v[1] * sr * std::sin (2 * kPi * v[2] * phase + v[3]));
                    outL += g * (v[4] <= 0 ? 1.0f : 0.35f) * s; outR += g * (v[4] >= 0 ? 1.0f : 0.35f) * s;
                }
            }
            if (cur.slap > 0)
            {
                const float o = slapD.read (slapSamples);
                slapD.push (sSendLP.process (sSendHP.process (x)) + 0.18f * o);
                outL += 0.55f * cur.slap * o; outR += 0.55f * cur.slap * o * 0.9f;
            }
            else slapD.push (0.0f);
            {
                const float oL = echoL.read (echoSamples), oR = echoR.read (echoSamples);
                echoLpL = echoLpL * 0.3f + oR * 0.7f; echoLpR = echoLpR * 0.3f + oL * 0.7f;   // ping-pong
                const float send = echoAmt > 0 ? eSendLP.process (eSendHP.process (x)) : 0.0f;
                echoL.push (send + 0.38f * echoLpL); echoR.push (0.38f * echoLpR);
                if (echoAmt > 0)
                {
                    const float dk = std::clamp ((10.0f * std::log10 ((float) duckE) - (duckRef - 14.0f)) / 10.0f, 0.0f, 1.0f);
                    const float wg = echoAmt * 0.6f * dbToG (-8.0f * dk);
                    outL += wg * oL; outR += wg * oR;
                }
            }

            // ---- output: sung parts at the chosen level, then a safety limiter (-1 dBFS)
            {
                // track the sung level of the finished sound (gaps ignored) and steer it to the chosen level
                const float m = 0.5f * (outL + outR);
                outE = aFast * outE + (1 - aFast) * m * m + 1e-20;
                const float ol = 10.0f * std::log10 ((float) outE);
                if (ol > outSung - 15.0f && ol > -70.0f)
                {
                    const double a = outSeen < 2.0 * sr ? aSungQuick : aSung;
                    outSung = (float) (a * outSung + (1 - a) * ol); outSeen += 1;
                }
                const float want = std::clamp (cur.level - 0.5f - outSung, -12.0f, 30.0f);
                const double ao = outSeen < 2.0 * sr ? aSungQuick : aOut;   // settle quickly at first
                outDb = (float) (ao * outDb + (1 - ao) * want);
                const float g = dbToG (outDb + rides.outDb);
                outL *= g; outR *= g;
                const float pk = std::max (std::abs (outL), std::abs (outR)), ceil = 0.891f;
                const float need = pk > ceil ? ceil / pk : 1.0f;
                limG = need < limG ? need : (float) (aLimR * limG + (1 - aLimR) * need);
                outL = std::clamp (outL * limG, -ceil, ceil); outR = std::clamp (outR * limG, -ceil, ceil);
            }

            if (stereoOut) { L[i] = outL; R[i] = outR; }
            else L[i] = 0.5f * (outL + outR);
        }
    }

    /** Longest tail the chain can ring on for (reverb and echo). */
    double tailSeconds() const { return cur.on ? std::max (0.3, (double) cur.size) * 1.4 + (cur.echo > 0 ? 2.5 : 0.0) : 0.0; }

private:
    void update()
    {
        const auto& s = cur;
        const double lc = std::max (40.0f, s.lowcut);
        hp1.set (Biquad::HP, lc, sr); hp2.set (Biquad::HP, lc, sr); hp3.set (Biquad::HP, lc * 0.9, sr, 0.54);
        // fitted to the editor's tone curves (within 0.3 dB)
        eqBody.set (Biquad::Peak, 180, sr, 0.84, s.body * 1.05); eqMud.set (Biquad::Peak, 350, sr, 1.09, s.mud * 1.06);
        eqPres.set (Biquad::Peak, 3040, sr, 0.9, s.presence * 1.05); eqAir.set (Biquad::HighShelf, 8090, sr, 0.5, s.air * 0.79);
        for (size_t k = 0; k < eqTone.size(); ++k)
        {
            toneOn[k] = s.hasToneEq && std::abs (s.toneEq[k]) > 0.05f && Settings::toneFreqs[k] < sr * 0.45;
            if (toneOn[k]) eqTone[k].set (Biquad::Peak, Settings::toneFreqs[k], sr, 4.32, s.toneEq[k]);
        }
        // the singer's own sibilance band (the editor measures where their "s" sounds peak), as in the editor
        const double dsFc = std::max (2500.0, std::min ({ 9000.0, sr * 0.4, (double) s.deessHz * 0.72 }));
        dsH1.set (Biquad::HP, dsFc, sr); dsH2.set (Biquad::HP, dsFc, sr);
        exH1.set (Biquad::HP, 3000, sr); exH2.set (Biquad::HP, 3000, sr); exH3.set (Biquad::HP, 4500, sr); exH4.set (Biquad::HP, 4500, sr);
        static constexpr float vtScale[4] = { 0.42f, 0.70f, 1.0f, 1.35f }, vtPre[4] = { 0.006f, 0.015f, 0.025f, 0.040f },
                               vtDamp[4] = { 0.30f, 0.38f, 0.20f, 0.45f }, vtLp[4] = { 8000, 7000, 9500, 6500 };
        const int t = std::clamp (s.verbType, 0, 3);
        vSendHP.set (Biquad::HP, 250, sr); vSendLP.set (Biquad::LP, vtLp[t], sr);
        preSamples = vtPre[t] * sr; damp = vtDamp[t];
        static constexpr int base[8] = { 1031, 1327, 1523, 1801, 2063, 2411, 2797, 3109 };
        const double rt = std::max (0.3, (double) s.size);
        for (size_t k = 0; k < 8; ++k)
        {
            fdnLen[k] = std::max (64.0, std::round (base[k] * sr / 44100.0 * (0.6 + 0.4 * s.size / 1.6) * vtScale[t]));
            fdnLen[k] = std::min (fdnLen[k], (double) fdn[k].len - 2);
            fdnFb[k] = (float) std::pow (10.0, -3.0 * fdnLen[k] / (rt * sr));
        }
        sSendHP.set (Biquad::HP, 200, sr); sSendLP.set (Biquad::LP, 4500, sr); slapSamples = 0.105 * sr;
        eSendHP.set (Biquad::HP, 350, sr); eSendLP.set (Biquad::LP, 5000, sr);
        const auto tc = [this] (double sec) { return std::exp (-1.0 / (sr * sec)); };
        aFast = tc (0.3); aSung = tc (8.0); aSungQuick = tc (0.6); aRide = tc (0.15);
        aGateE = tc (0.004); aOpen = tc (0.002); aClose = tc (0.12);
        aDet = tc (0.005); aAtt = tc (0.005); aRel = tc (0.09); aPunchA = tc (0.003); aPunchR = tc (0.15);
        aDs = tc (0.003); aDsR = tc (0.06);
        aDuckA = tc (0.01); aDuckRv = tc (0.25); aDuckRe = tc (0.3);
        aOut = tc (2.0); aLimR = tc (0.08);
    }

    double sr = 44100;
    Settings cur, pending; bool havePending = false, dirty = true;
    std::mutex pendingLock;

    Biquad hp1, hp2, hp3, eqBody, eqMud, eqPres, eqAir, dsH1, dsH2, exH1, exH2, exH3, exH4, vSendHP, vSendLP, sSendHP, sSendLP, eSendHP, eSendLP;
    std::array<Biquad, 25> eqTone; std::array<bool, 25> toneOn {};
    std::array<Delay, 8> fdn; double fdnLen[8] {}; float fdnFb[8] {}; double fdnLp[8] {};
    static constexpr float TL[8] = { 1, -1, 1, -1, 1, -1, 1, -1 }, TR[8] = { 1, 1, -1, -1, 1, 1, -1, -1 }, TI[8] = { 1, -1, -1, 1, 1, 1, -1, -1 };
    Delay preDelay, dblL, dblR, cho, slapD, echoL, echoR;
    double preSamples = 0, slapSamples = 0; float damp = 0.2f;

    double rmsFast = 0, rmsSlow = 0, compEnv = 0, punchEnv = 0, gateEnv = 0, dsEh = 0, dsEf = 0, duckV = 0, duckE = 0, phase = 0, sungSeen = 0;
    float outSung = -24; double outE = 0, outSeen = 0;
    float riderDb = 0, sungDb = -24, compGr = 0, punchGr = 0, gateDb = 0, dsG = 0, outDb = 6, limG = 1, echoLpL = 0, echoLpR = 0;
    int gateHold = 0;
    double aFast = 0, aSung = 0, aSungQuick = 0, aRide = 0, aGateE = 0, aOpen = 0, aClose = 0, aDet = 0, aAtt = 0, aRel = 0, aPunchA = 0, aPunchR = 0,
           aDs = 0, aDsR = 0, aDuckA = 0, aDuckRv = 0, aDuckRe = 0, aOut = 0, aLimR = 0;
};
} // namespace sudzfx

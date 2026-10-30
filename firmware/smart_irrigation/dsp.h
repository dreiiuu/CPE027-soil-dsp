// dsp.h - portable DSP core (no Arduino dependencies, so it also compiles on a PC).
// Implements the pipeline in the report:
//   Stage 1: cross-sensor median of 3 calibrated indices      (Eq. 3)
//   Stage 2: 5-point running median                           (Eq. 4)
//   Stage 3: exponential moving average (single-pole IIR)     (Eq. 5)
//   Baseline: M-point moving average                          (Eq. 6)
//   Decision: hysteresis + Tmax + lockout + blanking          (Eq. 7)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace dsp {

// ---------- calibration (Eq. 2) ----------
inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Relative moisture index 0..100 from a raw ADC count.
inline float toIndex(float adc, float adryCnt, float awetCnt) {
  float span = adryCnt - awetCnt;
  if (span < 1.0f) span = 1.0f;  // guard against a bad calibration
  return clampf(100.0f * (adryCnt - adc) / span, 0.0f, 100.0f);
}

// ---------- Stage 1: median of three ----------
inline float median3(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { b = a; }
  return b;
}

// ---------- Stage 2: N-point running median ----------
template <size_t N>
class RunningMedian {
 public:
  RunningMedian() : count_(0), head_(0) {}
  void reset() { count_ = 0; head_ = 0; }
  float update(float x) {
    buf_[head_] = x;
    head_ = (head_ + 1) % N;
    if (count_ < N) count_++;
    float tmp[N];
    for (size_t i = 0; i < count_; i++) tmp[i] = buf_[i];
    // insertion sort over the valid samples (N is tiny)
    for (size_t i = 1; i < count_; i++) {
      float v = tmp[i];
      size_t j = i;
      while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; }
      tmp[j] = v;
    }
    // Until the window fills, use the median of what we have so far.
    return tmp[count_ / 2];
  }
 private:
  float buf_[N];
  size_t count_, head_;
};

// ---------- Stage 3: EMA ----------
class Ema {
 public:
  explicit Ema(float alpha = 0.1f) : a_(alpha), y_(0), init_(false) {}
  void setAlpha(float a) { a_ = a; }
  void reset() { init_ = false; }
  float update(float x) {
    if (!init_) { y_ = x; init_ = true; }  // seed with first sample: no startup ramp
    else y_ = a_ * x + (1.0f - a_) * y_;
    return y_;
  }
  float value() const { return y_; }
 private:
  float a_, y_;
  bool init_;
};

// ---------- Baseline: M-point moving average ----------
template <size_t M>
class MovingAverage {
 public:
  MovingAverage() : count_(0), head_(0), sum_(0) {}
  void reset() { count_ = 0; head_ = 0; sum_ = 0; }
  float update(float x) {
    if (count_ == M) sum_ -= buf_[head_]; else count_++;
    buf_[head_] = x;
    sum_ += x;
    head_ = (head_ + 1) % M;
    return sum_ / (float)count_;
  }
 private:
  float buf_[M];
  size_t count_, head_;
  float sum_;
};

// ---------- whole pipeline ----------
struct PipelineOut {
  float m1, m2, m3;  // calibrated per-sensor indices
  float x;           // cross-sensor median (Stage 1)
  float z;           // running median (Stage 2)
  float y;           // proposed output (Stage 3)
  float yma;         // moving-average baseline, same input x
};

template <size_t N = 5, size_t M = 10>
class Pipeline {
 public:
  explicit Pipeline(float alpha = 0.1f) : ema_(alpha) {}
  void setAlpha(float a) { ema_.setAlpha(a); }
  void reset() { rm_.reset(); ema_.reset(); ma_.reset(); }
  PipelineOut update(float m1, float m2, float m3) {
    PipelineOut o;
    o.m1 = m1; o.m2 = m2; o.m3 = m3;
    o.x = median3(m1, m2, m3);
    o.z = rm_.update(o.x);
    o.y = ema_.update(o.z);
    o.yma = ma_.update(o.x);
    return o;
  }
 private:
  RunningMedian<N> rm_;
  Ema ema_;
  MovingAverage<M> ma_;
};

// ---------- decision logic (Eq. 7) ----------
enum class Status : uint8_t { DRY = 0, OK = 1, WET = 2 };

struct Params {
  float thetaDry = 35.0f;     // pump starts below this index
  float hysteresis = 8.0f;    // pump stops at thetaDry + h
  float thetaWet = 80.0f;     // wet alert above this index
  uint32_t tMaxMs = 15000;    // max continuous pump run
  uint32_t lockoutMs = 120000;// wait after watering before judging again
  uint32_t blankMs = 3000;    // ignore sensors this long after a relay switch
};

class Controller {
 public:
  explicit Controller(const Params& p = Params()) : p_(p), pump_(false),
      tOn_(0), tOff_(0), everOff_(false), wet_(false), status_(Status::OK) {}
  void setParams(const Params& p) { p_ = p; }
  const Params& params() const { return p_; }

  // y = smoothed index, nowMs = monotonic ms, enabled = pump enable flag.
  // Returns the pump state AFTER this update.
  bool update(float y, uint32_t nowMs, bool enabled = true) {
    wet_ = y > p_.thetaWet;
    if (!enabled) {
      if (pump_) { pump_ = false; tOff_ = nowMs; everOff_ = true; }
    } else if (pump_) {
      if (y >= p_.thetaDry + p_.hysteresis || (nowMs - tOn_) >= p_.tMaxMs) {
        pump_ = false; tOff_ = nowMs; everOff_ = true;
      }
    } else {
      bool lockedOut = everOff_ && (nowMs - tOff_) < p_.lockoutMs;
      if (y < p_.thetaDry && !lockedOut) { pump_ = true; tOn_ = nowMs; }
    }
    if (wet_) status_ = Status::WET;
    else if (y < p_.thetaDry) status_ = Status::DRY;
    else status_ = Status::OK;
    return pump_;
  }
  bool pump() const { return pump_; }
  bool wet() const { return wet_; }
  Status status() const { return status_; }
 private:
  Params p_;
  bool pump_;
  uint32_t tOn_, tOff_;
  bool everOff_, wet_;
  Status status_;
};

// Decision-toggle counter for a fixed threshold (used on the dashboard and
// mirrored in analyze_log.py). A toggle = the signal crosses the threshold
// with hysteresis band [thr-h/2, thr+h/2]; h = 0 gives a plain crossing count.
class ToggleCounter {
 public:
  explicit ToggleCounter(float thr = 35.0f, float h = 0.0f)
      : thr_(thr), h_(h), state_(-1), count_(0) {}
  void reset() { state_ = -1; count_ = 0; }
  void setThreshold(float thr) { thr_ = thr; }
  uint32_t update(float v) {
    int s = state_;
    if (state_ < 0) s = (v >= thr_) ? 1 : 0;
    else if (state_ == 1 && v < thr_ - h_ * 0.5f) s = 0;
    else if (state_ == 0 && v >= thr_ + h_ * 0.5f) s = 1;
    if (state_ >= 0 && s != state_) count_++;
    state_ = s;
    return count_;
  }
  uint32_t count() const { return count_; }
 private:
  float thr_, h_;
  int state_;
  uint32_t count_;
};

}  // namespace dsp

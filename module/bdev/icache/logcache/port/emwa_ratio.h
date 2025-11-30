#pragma once
#include "emwa.h"

class EwmaRatio {
public:
    explicit EwmaRatio(const Ewma& base) : ema_(base), prevH_(0), prevUc_(0), initialized_(false) {}

    static EwmaRatio FromHalfLifeBlocks(double hl_blocks, bool bias_correction = true) {
        return EwmaRatio(Ewma::FromHalfLifeBlocks(hl_blocks, bias_correction));
    }

    // 직접 Δ를 주는 버전 (기존)
    void update(long double numer_inc, long double denom_inc) {
        if (denom_inc <= 0.0L) return; // skip
        double x = static_cast<double>(numer_inc / denom_inc);
        ema_.updateWithBlocks(x, static_cast<double>(denom_inc));
    }

    // 누적값(H, Uc) 넣으면 내부에서 Δ를 계산해 update
    void updateFromCumulative(long double H, long double Uc) {
        if (!initialized_) {
            prevH_  = H;
            prevUc_ = Uc;
            initialized_ = true;
            return; // 첫 샘플은 baseline만 저장
        }
        long double dH  = H  - prevH_;
        long double dUc = Uc - prevUc_;
        prevH_  = H;
        prevUc_ = Uc;
        if (dH > 0.0L) {
            update(dUc, dH);
        }
    }

    double value() const { return ema_.value(); }
    bool has_value() const { return ema_.has_value(); }

private:
    Ewma ema_;
    long double prevH_, prevUc_;
    bool initialized_;
};
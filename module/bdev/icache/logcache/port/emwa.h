// ewma.h — Exponentially Weighted Moving Average (EWMA)
#pragma once

#include <cstdint>

class Ewma {
public:
    explicit Ewma(double alpha,
                  bool bias_correction = false,
                  double base_interval_units = 1.0);

    // 고정된 "스텝" 길이 N 기반 (alpha = 2/(N+1))
    static Ewma FromWindow(std::size_t N, bool bias_correction = false);

    // 하프라이프(단위 = base_interval_units) 기반
    static Ewma FromHalfLife(double half_life_units,
                             bool bias_correction = false,
                             double base_interval_units = 1.0);

    // "블록"을 1 단위로 보고, 하프라이프를 블록 수로 지정
    static Ewma FromHalfLifeBlocks(double half_life_blocks,
                                   bool bias_correction = false) {
        return FromHalfLife(half_life_blocks, bias_correction, /*base_interval_units=*/1.0);
    }

    void reset();

    // 한 단위(step) 업데이트
    void update(double x);

    // 임의의 경과 "단위 수"로 가중 업데이트 (가중치=units)
    void updateWithUnits(double x, double units);

    // 편의: 블록 수로 가중 업데이트 (units = written_blocks)
    void updateWithBlocks(double x, double written_blocks) {
        updateWithUnits(x, written_blocks);
    }

    // 현재 값 (bias-corrected 선택)
    double value() const;

    // 디버그용 raw
    double raw() const { return initialized_ ? m_ : __builtin_nan(""); }

    bool has_value() const { return initialized_; }

    double alpha() const { return alpha_; }
    double base_interval_units() const { return base_interval_units_; }
    bool bias_correction() const { return bias_correction_; }
    std::uint64_t steps() const { return steps_; }
private:
    static double clampAlpha(double a);
    void updateWithAlpha(double x, double alpha_eff);

    double        alpha_;
    bool          bias_correction_;
    double        base_interval_units_;  // 이제 "초"가 아니라 임의의 단위(여기선 블록)
    bool          initialized_;
    double        m_;
    std::uint64_t steps_;
    double        bias_prod_; // ∏(1 - alpha_k)
};
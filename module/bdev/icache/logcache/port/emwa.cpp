// ewma.cpp
#include "emwa.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr double kEpsTiny   = 1e-12;
constexpr double kProdFloor = 1e-15;
constexpr double kLn2       = 0.69314718055994530942;

static double
clamp_value(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}
}

Ewma::Ewma(double alpha, bool bias_correction, double base_interval_units)
    : alpha_(clampAlpha(alpha)),
      bias_correction_(bias_correction),
      base_interval_units_(base_interval_units > 0 ? base_interval_units : 1.0),
      initialized_(false),
      m_(0.0),
      steps_(0),
      bias_prod_(1.0) {}

Ewma Ewma::FromWindow(std::size_t N, bool bias_correction) {
    if (N == 0) throw std::invalid_argument("N must be > 0");
    double alpha = 2.0 / (static_cast<double>(N) + 1.0);
    return Ewma(alpha, bias_correction, /*base_interval_units=*/1.0);
}

Ewma Ewma::FromHalfLife(double half_life_units, bool bias_correction, double base_interval_units) {
    if (half_life_units <= 0.0)    throw std::invalid_argument("half_life_units must be > 0");
    if (base_interval_units <= 0.) throw std::invalid_argument("base_interval_units must be > 0");
    double alpha = 1.0 - std::exp(-kLn2 * (base_interval_units / half_life_units));
    alpha = clamp_value(alpha, kEpsTiny, 1.0);
    return Ewma(alpha, bias_correction, base_interval_units);
}

void Ewma::reset() {
    initialized_ = false;
    m_ = 0.0;
    steps_ = 0;
    bias_prod_ = 1.0;
}

// Ewma::update — now inline in emwa.h (hot per-event path).

void Ewma::updateWithUnits(double x, double units) {
    if (units <= 0.0) throw std::invalid_argument("units must be > 0");
    const double k = units / base_interval_units_;
    const double one_minus_alpha = (1.0 - alpha_);
    const double decay = std::exp(k * std::log(std::max(kProdFloor, one_minus_alpha))); // (1-α)^k
    double alpha_eff = 1.0 - decay;
    alpha_eff = clamp_value(alpha_eff, kEpsTiny, 1.0);
    updateWithAlpha(x, alpha_eff);
    bias_prod_ *= (1.0 - alpha_eff);
    steps_ += 1;
}

double Ewma::value() const {
    if (!initialized_) return std::numeric_limits<double>::quiet_NaN();
    if (!bias_correction_) return m_;
    double denom = 1.0 - bias_prod_;
    if (denom <= kProdFloor) return m_; // 초기 폭주 방지
    return m_ / denom;
}

double Ewma::clampAlpha(double a) {
    if (!(a > 0.0 && a <= 1.0)) throw std::invalid_argument("alpha must be in (0,1]");
    return a;
}

// Ewma::updateWithAlpha — now inline in emwa.h.

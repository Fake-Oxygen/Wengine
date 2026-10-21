#pragma once

#include <ostream>

namespace wengine::units {

// Compile-time SI dimension: Length, Time, Mass, Current, Temp, Amount, Light.
template <int L, int T, int M, int I, int K, int N, int C>
struct Dimension {
  static constexpr int length = L;
  static constexpr int time = T;
  static constexpr int mass = M;
  static constexpr int current = I;
  static constexpr int temperature = K;
  static constexpr int amount = N;
  static constexpr int light = C;
};

using Length = Dimension<1, 0, 0, 0, 0, 0, 0>;
using Time = Dimension<0, 1, 0, 0, 0, 0, 0>;
using Mass = Dimension<0, 0, 1, 0, 0, 0, 0>;
using Energy = Dimension<2, -2, 1, 0, 0, 0, 0>;
using Power = Dimension<2, -3, 1, 0, 0, 0, 0>;
using ElectricPotential = Dimension<2, -3, 1, -1, 0, 0, 0>;
using ElectricCurrent = Dimension<0, 0, 0, 1, 0, 0, 0>;
using ElectricCharge = Dimension<0, 1, 0, 1, 0, 0, 0>;
using ElectricResistance = Dimension<2, -3, 1, -2, 0, 0, 0>;
using Frequency = Dimension<0, -1, 0, 0, 0, 0, 0>;
using Count = Dimension<0, 0, 0, 0, 0, 0, 0>; // dimensionless: pulse / impulse counts
using MeterConstant = Dimension<-2, 2, -1, 0, 0, 0, 0>; // Count / Energy (imp per joule, SI base)
using Capacitance = Dimension<-2, 4, -1, 2, 0, 0, 0>; // s^4*A^2 / (kg*m^2), base unit F

template <typename Dim1, typename Dim2>
using DimensionMultiply = Dimension<Dim1::length + Dim2::length, Dim1::time + Dim2::time,
                                   Dim1::mass + Dim2::mass, Dim1::current + Dim2::current,
                                   Dim1::temperature + Dim2::temperature,
                                   Dim1::amount + Dim2::amount, Dim1::light + Dim2::light>;

template <typename Dim1, typename Dim2>
using DimensionDivide = Dimension<Dim1::length - Dim2::length, Dim1::time - Dim2::time,
                                 Dim1::mass - Dim2::mass, Dim1::current - Dim2::current,
                                 Dim1::temperature - Dim2::temperature,
                                 Dim1::amount - Dim2::amount, Dim1::light - Dim2::light>;

template <typename Dim>
class Value {
public:
  explicit constexpr Value(double val = 0.0) noexcept : value_(val) {}
  [[nodiscard]] constexpr double get() const noexcept { return value_; }
  // Legacy spelling.
  [[nodiscard]] constexpr double GetValue() const noexcept { return value_; }

  constexpr Value<Dim> operator+(const Value<Dim> &rhs) const noexcept {
    return Value<Dim>(value_ + rhs.value_);
  }
    constexpr Value<Dim> operator-(const Value<Dim> &rhs) const noexcept {
      return Value<Dim>(value_ - rhs.value_);
    }

    // Exact floating-point comparisons (same dimension only); use .get()
    // with an epsilon for tolerance-based checks.
    constexpr bool operator==(const Value<Dim> &rhs) const noexcept {
      return value_ == rhs.value_;
    }
    constexpr bool operator!=(const Value<Dim> &rhs) const noexcept {
      return value_ != rhs.value_;
    }
    constexpr bool operator<(const Value<Dim> &rhs) const noexcept {
      return value_ < rhs.value_;
    }
    constexpr bool operator<=(const Value<Dim> &rhs) const noexcept {
      return value_ <= rhs.value_;
    }
    constexpr bool operator>(const Value<Dim> &rhs) const noexcept {
      return value_ > rhs.value_;
    }
    constexpr bool operator>=(const Value<Dim> &rhs) const noexcept {
      return value_ >= rhs.value_;
    }

  template <typename OtherDim>
  constexpr auto operator*(const Value<OtherDim> &rhs) const noexcept {
    using NewDim = DimensionMultiply<Dim, OtherDim>;
    return Value<NewDim>(value_ * rhs.get());
  }

  template <typename OtherDim>
  constexpr auto operator/(const Value<OtherDim> &rhs) const noexcept {
    using NewDim = DimensionDivide<Dim, OtherDim>;
    return Value<NewDim>(value_ / rhs.get());
  }

private:
  double value_;
};

// Back-compat alias for legacy code (Physics::WValue).
template <typename Dim>
using WValue = Value<Dim>;

// Prints the raw SI base value (no unit suffix); callers add context.
template <typename Dim>
std::ostream &operator<<(std::ostream &out, const Value<Dim> &v) {
  return out << v.get();
}

} // namespace wengine::units

namespace wengine::units::literals {

constexpr Value<ElectricCurrent> operator""_A(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val));
}

constexpr Value<ElectricCurrent> operator""_mA(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val) * 1e-3);
}

constexpr Value<ElectricCurrent> operator""_uA(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val) * 1e-6);
}

constexpr Value<ElectricResistance> operator""_Ohm(long double val) {
  return Value<ElectricResistance>(static_cast<double>(val));
}

constexpr Value<ElectricResistance> operator""_kOhm(long double val) {
  return Value<ElectricResistance>(static_cast<double>(val) * 1e3);
}

constexpr Value<ElectricPotential> operator""_V(long double val) {
  return Value<ElectricPotential>(static_cast<double>(val));
}

constexpr Value<Power> operator""_W(long double val) {
  return Value<Power>(static_cast<double>(val));
}

constexpr Value<Power> operator""_kW(long double val) {
  return Value<Power>(static_cast<double>(val) * 1000.0);
}

constexpr Value<Energy> operator""_J(long double val) {
  return Value<Energy>(static_cast<double>(val));
}

constexpr Value<Energy> operator""_Wh(long double val) {
  return Value<Energy>(static_cast<double>(val) * 3600.0);
}

constexpr Value<Energy> operator""_kWh(long double val) {
  return Value<Energy>(static_cast<double>(val) * 3600000.0);
}

constexpr Value<Frequency> operator""_Hz(long double val) {
  return Value<Frequency>(static_cast<double>(val));
}

constexpr Value<Frequency> operator""_MHz(long double val) {
  return Value<Frequency>(static_cast<double>(val) * 1e6);
}

constexpr Value<Frequency> operator""_rpm(long double val) {
  return Value<Frequency>(static_cast<double>(val) / 60.0);
}

constexpr Value<Frequency> operator""_rps(long double val) {
  return Value<Frequency>(static_cast<double>(val));
}

constexpr Value<Time> operator""_s(long double val) {
  return Value<Time>(static_cast<double>(val));
}

constexpr Value<Time> operator""_ms(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-3);
}

constexpr Value<Time> operator""_us(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-6);
}

constexpr Value<Time> operator""_ns(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-9);
}

constexpr Value<Time> operator""_h(long double val) {
  return Value<Time>(static_cast<double>(val) * 3600.0);
}

constexpr Value<Count> operator""_imp(long double val) {
  return Value<Count>(static_cast<double>(val));
}

constexpr Value<Capacitance> operator""_F(long double val) {
  return Value<Capacitance>(static_cast<double>(val));
}

constexpr Value<Capacitance> operator""_mF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-3);
}

constexpr Value<Capacitance> operator""_uF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-6);
}

constexpr Value<Capacitance> operator""_nF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-9);
}

constexpr Value<Capacitance> operator""_pF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-12);
}

} // namespace wengine::units::literals


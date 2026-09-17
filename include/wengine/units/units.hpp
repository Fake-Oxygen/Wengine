#pragma once

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

} // namespace wengine::units

namespace wengine::units::literals {

constexpr Value<ElectricCurrent> operator""_A(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val));
}

constexpr Value<ElectricResistance> operator""_Ohm(long double val) {
  return Value<ElectricResistance>(static_cast<double>(val));
}

constexpr Value<ElectricPotential> operator""_V(long double val) {
  return Value<ElectricPotential>(static_cast<double>(val));
}

} // namespace wengine::units::literals


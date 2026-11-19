#pragma once

/**
 * @file
 * Single source of truth for typed physical quantities: the compile-time
 * SI Dimension algebra, the Value<Dim> quantity type with its
 * arithmetic/comparison operators and stream output, and the UDL
 * literals. Values are SI base units internally; suffix/text knowledge
 * lives only in units/scales.hpp (data) and cli/args.hpp (generic
 * parsing) — this header must never gain parsing machinery or ad-hoc
 * conversion tables, and individual solvers must never encode their own.
 * Consumers are every engine module (solvers, world, signal) plus CLI
 * and GUI. The literals live in wengine::units::literals and must stay
 * there: a using-directive into another header would collide (for
 * example operator""_s).
 */

#include <ostream>

namespace wengine::units {

/**
 * @brief Compile-time SI dimension: exponents of the seven SI base units.
 *
 * @note Pure type-level metadata: dimensional algebra operates on the
 *       exponent constants; no runtime unit data ever exists.
 */
// Compile-time SI dimension: Length, Time, Mass, Current, Temp, Amount, Light.
template <int L, int T, int M, int I, int K, int N, int C>
struct Dimension {
  /**
   * @brief Exponent of the SI base unit metre (m).
   */
  static constexpr int length = L;
  /**
   * @brief Exponent of the SI base unit second (s).
   */
  static constexpr int time = T;
  /**
   * @brief Exponent of the SI base unit kilogram (kg).
   */
  static constexpr int mass = M;
  /**
   * @brief Exponent of the SI base unit ampere (A).
   */
  static constexpr int current = I;
  /**
   * @brief Exponent of the SI base unit kelvin (K).
   */
  static constexpr int temperature = K;
  /**
   * @brief Exponent of the SI base unit mole (mol).
   */
  static constexpr int amount = N;
  /**
   * @brief Exponent of the SI base unit candela (cd).
   */
  static constexpr int light = C;
};

/**
 * @brief Dimension of length; SI base unit metre (m).
 */
using Length = Dimension<1, 0, 0, 0, 0, 0, 0>;
/**
 * @brief Dimension of time; SI base unit second (s).
 */
using Time = Dimension<0, 1, 0, 0, 0, 0, 0>;
/**
 * @brief Dimension of mass; SI base unit kilogram (kg).
 */
using Mass = Dimension<0, 0, 1, 0, 0, 0, 0>;
/**
 * @brief Dimension of energy; SI base kg*m^2/s^2, practical unit joule (J).
 */
using Energy = Dimension<2, -2, 1, 0, 0, 0, 0>;
/**
 * @brief Dimension of power; SI base kg*m^2/s^3, practical unit watt (W).
 */
using Power = Dimension<2, -3, 1, 0, 0, 0, 0>;
/**
 * @brief Dimension of electric potential; SI base kg*m^2/(s^3*A),
 *        practical unit volt (V).
 */
using ElectricPotential = Dimension<2, -3, 1, -1, 0, 0, 0>;
/**
 * @brief Dimension of electric current; SI base unit ampere (A).
 */
using ElectricCurrent = Dimension<0, 0, 0, 1, 0, 0, 0>;
/**
 * @brief Dimension of electric charge; SI base A*s, practical unit
 *        coulomb (C).
 */
using ElectricCharge = Dimension<0, 1, 0, 1, 0, 0, 0>;
/**
 * @brief Dimension of electric resistance; SI base kg*m^2/(s^3*A^2),
 *        practical unit ohm.
 */
using ElectricResistance = Dimension<2, -3, 1, -2, 0, 0, 0>;
/**
 * @brief Dimension of frequency; SI base 1/s, practical unit hertz (Hz).
 */
using Frequency = Dimension<0, -1, 0, 0, 0, 0, 0>;
/**
 * @brief Dimensionless counts: pulse / impulse counts.
 */
using Count = Dimension<0, 0, 0, 0, 0, 0, 0>; // dimensionless: pulse / impulse counts
/**
 * @brief Dimension of a meter constant: Count / Energy (impulses per
 *        joule, SI base; canonical practical unit imp/kWh).
 */
using MeterConstant = Dimension<-2, 2, -1, 0, 0, 0, 0>; // Count / Energy (imp per joule, SI base)
/**
 * @brief Dimension of capacitance; SI base s^4*A^2/(kg*m^2), practical
 *        unit farad (F).
 */
using Capacitance = Dimension<-2, 4, -1, 2, 0, 0, 0>; // s^4*A^2 / (kg*m^2), base unit F

/**
 * @brief Alias template computing the product dimension: exponents of
 *        Dim1 and Dim2 add.
 */
template <typename Dim1, typename Dim2>
using DimensionMultiply = Dimension<Dim1::length + Dim2::length, Dim1::time + Dim2::time,
                                   Dim1::mass + Dim2::mass, Dim1::current + Dim2::current,
                                   Dim1::temperature + Dim2::temperature,
                                   Dim1::amount + Dim2::amount, Dim1::light + Dim2::light>;

/**
 * @brief Alias template computing the quotient dimension: exponents of
 *        Dim2 subtract from Dim1.
 */
template <typename Dim1, typename Dim2>
using DimensionDivide = Dimension<Dim1::length - Dim2::length, Dim1::time - Dim2::time,
                                 Dim1::mass - Dim2::mass, Dim1::current - Dim2::current,
                                 Dim1::temperature - Dim2::temperature,
                                 Dim1::amount - Dim2::amount, Dim1::light - Dim2::light>;

/**
 * @brief A physical quantity: a raw SI-base double tagged with a
 *        compile-time Dimension.
 *
 * @note The stored double is always the SI base value of Dim (volts,
 *       amperes, seconds, ...); scale knowledge ("mV", "kOhm") exists
 *       only in units/scales.hpp for parsing and display. Arithmetic is
 *       plain double arithmetic with no rounding or normalization, and
 *       comparisons are exact bit comparisons — use get() with a
 *       justified epsilon where inexact arithmetic is involved.
 */
template <typename Dim>
class Value {
public:
  /**
   * @brief Construct from a raw SI base value.
   *
   * @param[in] val magnitude in the SI base unit of Dim (e.g. _V for
   *            ElectricPotential, _A for ElectricCurrent, _s for Time).
   */
  explicit constexpr Value(double val = 0.0) noexcept : value_(val) {}
  /**
   * @brief Raw SI base value of the quantity.
   *
   * @return the magnitude in the SI base unit of Dim (meaning named by
   *         the dimension, e.g. _V, _A, _s).
   */
  [[nodiscard]] constexpr double get() const noexcept { return value_; }
  /**
   * @brief Legacy spelling of get() (Physics::WValue era).
   *
   * @return Raw SI base value of the quantity.
   */
  // Legacy spelling.
  [[nodiscard]] constexpr double GetValue() const noexcept { return value_; }

  /**
   * @brief Sum of two same-dimension quantities.
   *
   * @param[in] rhs addend of the same Dimension.
   * @return Value<Dim> whose SI base double is the plain sum.
   */
  constexpr Value<Dim> operator+(const Value<Dim> &rhs) const noexcept {
    return Value<Dim>(value_ + rhs.value_);
  }
    /**
     * @brief Difference of two same-dimension quantities.
     *
     * @param[in] rhs subtrahend of the same Dimension.
     * @return Value<Dim> whose SI base double is the plain difference.
     */
    constexpr Value<Dim> operator-(const Value<Dim> &rhs) const noexcept {
      return Value<Dim>(value_ - rhs.value_);
    }

    /**
     * @brief Exact equality of two same-dimension quantities.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when the SI base doubles are bit-identical.
     * @note Exact floating-point comparison; use .get() with an
     *       epsilon for tolerance-based checks.
     */
    // Exact floating-point comparisons (same dimension only); use .get()
    // with an epsilon for tolerance-based checks.
    constexpr bool operator==(const Value<Dim> &rhs) const noexcept {
      return value_ == rhs.value_;
    }
    /**
     * @brief Exact inequality of two same-dimension quantities.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when the SI base doubles differ.
     */
    constexpr bool operator!=(const Value<Dim> &rhs) const noexcept {
      return value_ != rhs.value_;
    }
    /**
     * @brief Exact ordering: less than.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when this SI base double is < rhs's.
     */
    constexpr bool operator<(const Value<Dim> &rhs) const noexcept {
      return value_ < rhs.value_;
    }
    /**
     * @brief Exact ordering: less than or equal.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when this SI base double is <= rhs's.
     */
    constexpr bool operator<=(const Value<Dim> &rhs) const noexcept {
      return value_ <= rhs.value_;
    }
    /**
     * @brief Exact ordering: greater than.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when this SI base double is > rhs's.
     */
    constexpr bool operator>(const Value<Dim> &rhs) const noexcept {
      return value_ > rhs.value_;
    }
    /**
     * @brief Exact ordering: greater than or equal.
     *
     * @param[in] rhs comparand of the same Dimension.
     * @return true when this SI base double is >= rhs's.
     */
    constexpr bool operator>=(const Value<Dim> &rhs) const noexcept {
      return value_ >= rhs.value_;
    }

  /**
   * @brief Dimensioned product of two quantities.
   *
   * @param[in] rhs factor of any Dimension.
   * @return Value of dimension DimensionMultiply<Dim, OtherDim> whose
   *         SI base double is the plain product.
   */
  template <typename OtherDim>
  constexpr auto operator*(const Value<OtherDim> &rhs) const noexcept {
    using NewDim = DimensionMultiply<Dim, OtherDim>;
    return Value<NewDim>(value_ * rhs.get());
  }

  /**
   * @brief Dimensioned quotient of two quantities.
   *
   * @param[in] rhs divisor of any Dimension.
   * @return Value of dimension DimensionDivide<Dim, OtherDim> whose
   *         SI base double is the plain quotient.
   */
  template <typename OtherDim>
  constexpr auto operator/(const Value<OtherDim> &rhs) const noexcept {
    using NewDim = DimensionDivide<Dim, OtherDim>;
    return Value<NewDim>(value_ / rhs.get());
  }

private:
  double value_;
};

/**
 * @brief Back-compat alias for legacy code (Physics::WValue).
 *
 * @note New code spells the type Value<Dim> directly.
 */
// Back-compat alias for legacy code (Physics::WValue).
template <typename Dim>
using WValue = Value<Dim>;

/**
 * @brief Stream out the raw SI base value (no unit suffix).
 *
 * @param[in] out target stream.
 * @param[in] v quantity to print.
 * @return `out` after printing v.get().
 * @note Callers add the unit context; no suffix knowledge lives here.
 */
// Prints the raw SI base value (no unit suffix); callers add context.
template <typename Dim>
std::ostream &operator<<(std::ostream &out, const Value<Dim> &v) {
  return out << v.get();
}

} // namespace wengine::units

/**
 * @brief User-defined literals for quantities (e.g. 12_V, 3_mA, 0.5_s).
 *
 * @note The literal operators MUST stay inside this namespace: a
 *       using-directive at header scope would collide with other
 *       literal spellings (operator""_s is the known clash). Opt in
 *       explicitly with `using namespace wengine::units::literals;`
 *       at .cpp scope only, never in a header.
 */
namespace wengine::units::literals {

/**
 * @brief Amperes literal: `1.5_A` is 1.5 A.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricCurrent> carrying amperes (SI base).
 */
constexpr Value<ElectricCurrent> operator""_A(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val));
}

/**
 * @brief Milliamperes literal: `20_mA` is 0.02 A.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricCurrent> carrying val * 1e-3 amperes (SI base).
 */
constexpr Value<ElectricCurrent> operator""_mA(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val) * 1e-3);
}

/**
 * @brief Microamperes literal: `10_uA` is 10e-6 A.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricCurrent> carrying val * 1e-6 amperes (SI base).
 */
constexpr Value<ElectricCurrent> operator""_uA(long double val) {
  return Value<ElectricCurrent>(static_cast<double>(val) * 1e-6);
}

/**
 * @brief Ohms literal: `470_Ohm` is 470 ohm.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricResistance> carrying ohms (SI base).
 */
constexpr Value<ElectricResistance> operator""_Ohm(long double val) {
  return Value<ElectricResistance>(static_cast<double>(val));
}

/**
 * @brief Kiloohms literal: `4_kOhm` is 4000 ohm.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricResistance> carrying val * 1e3 ohms (SI base).
 */
constexpr Value<ElectricResistance> operator""_kOhm(long double val) {
  return Value<ElectricResistance>(static_cast<double>(val) * 1e3);
}

/**
 * @brief Volts literal: `12_V` is 12 V.
 *
 * @param[in] val literal digits.
 * @return Value<ElectricPotential> carrying volts (SI base).
 */
constexpr Value<ElectricPotential> operator""_V(long double val) {
  return Value<ElectricPotential>(static_cast<double>(val));
}

/**
 * @brief Watts literal: `60_W` is 60 W.
 *
 * @param[in] val literal digits.
 * @return Value<Power> carrying watts (SI base).
 */
constexpr Value<Power> operator""_W(long double val) {
  return Value<Power>(static_cast<double>(val));
}

/**
 * @brief Kilowatts literal: `1.5_kW` is 1500 W.
 *
 * @param[in] val literal digits.
 * @return Value<Power> carrying val * 1000 watts (SI base).
 */
constexpr Value<Power> operator""_kW(long double val) {
  return Value<Power>(static_cast<double>(val) * 1000.0);
}

/**
 * @brief Joules literal: `3_J` is 3 J.
 *
 * @param[in] val literal digits.
 * @return Value<Energy> carrying joules (SI base).
 */
constexpr Value<Energy> operator""_J(long double val) {
  return Value<Energy>(static_cast<double>(val));
}

/**
 * @brief Watt-hours literal: `1_Wh` is 3600 J.
 *
 * @param[in] val literal digits.
 * @return Value<Energy> carrying val * 3600 joules (SI base).
 */
constexpr Value<Energy> operator""_Wh(long double val) {
  return Value<Energy>(static_cast<double>(val) * 3600.0);
}

/**
 * @brief Kilowatt-hours literal: `1_kWh` is 3.6e6 J.
 *
 * @param[in] val literal digits.
 * @return Value<Energy> carrying val * 3.6e6 joules (SI base).
 */
constexpr Value<Energy> operator""_kWh(long double val) {
  return Value<Energy>(static_cast<double>(val) * 3600000.0);
}

/**
 * @brief Hertz literal: `50_Hz` is 50 Hz.
 *
 * @param[in] val literal digits.
 * @return Value<Frequency> carrying hertz (SI base 1/s).
 */
constexpr Value<Frequency> operator""_Hz(long double val) {
  return Value<Frequency>(static_cast<double>(val));
}

/**
 * @brief Megahertz literal: `2_MHz` is 2e6 Hz.
 *
 * @param[in] val literal digits.
 * @return Value<Frequency> carrying val * 1e6 hertz (SI base 1/s).
 */
constexpr Value<Frequency> operator""_MHz(long double val) {
  return Value<Frequency>(static_cast<double>(val) * 1e6);
}

/**
 * @brief Revolutions-per-minute literal: `300_rpm` is 5 Hz.
 *
 * @param[in] val literal digits.
 * @return Value<Frequency> carrying val / 60 hertz (SI base 1/s).
 */
constexpr Value<Frequency> operator""_rpm(long double val) {
  return Value<Frequency>(static_cast<double>(val) / 60.0);
}

/**
 * @brief Revolutions-per-second literal: `5_rps` is 5 Hz.
 *
 * @param[in] val literal digits.
 * @return Value<Frequency> carrying hertz (SI base 1/s).
 */
constexpr Value<Frequency> operator""_rps(long double val) {
  return Value<Frequency>(static_cast<double>(val));
}

/**
 * @brief Seconds literal: `0.01_s` is 0.01 s.
 *
 * @param[in] val literal digits.
 * @return Value<Time> carrying seconds (SI base).
 */
constexpr Value<Time> operator""_s(long double val) {
  return Value<Time>(static_cast<double>(val));
}

/**
 * @brief Milliseconds literal: `10_ms` is 0.01 s.
 *
 * @param[in] val literal digits.
 * @return Value<Time> carrying val * 1e-3 seconds (SI base).
 */
constexpr Value<Time> operator""_ms(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-3);
}

/**
 * @brief Microseconds literal: `100_us` is 1e-4 s.
 *
 * @param[in] val literal digits.
 * @return Value<Time> carrying val * 1e-6 seconds (SI base).
 */
constexpr Value<Time> operator""_us(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-6);
}

/**
 * @brief Nanoseconds literal: `500_ns` is 5e-7 s.
 *
 * @param[in] val literal digits.
 * @return Value<Time> carrying val * 1e-9 seconds (SI base).
 */
constexpr Value<Time> operator""_ns(long double val) {
  return Value<Time>(static_cast<double>(val) * 1e-9);
}

/**
 * @brief Hours literal: `1_h` is 3600 s.
 *
 * @param[in] val literal digits.
 * @return Value<Time> carrying val * 3600 seconds (SI base).
 */
constexpr Value<Time> operator""_h(long double val) {
  return Value<Time>(static_cast<double>(val) * 3600.0);
}

/**
 * @brief Impulse-count literal: `800_imp` is the plain count 800.
 *
 * @param[in] val literal digits.
 * @return Value<Count> carrying the dimensionless count.
 */
constexpr Value<Count> operator""_imp(long double val) {
  return Value<Count>(static_cast<double>(val));
}

/**
 * @brief Farads literal: `100_F` is 100 F.
 *
 * @param[in] val literal digits.
 * @return Value<Capacitance> carrying farads (SI base).
 */
constexpr Value<Capacitance> operator""_F(long double val) {
  return Value<Capacitance>(static_cast<double>(val));
}

/**
 * @brief Millifarads literal: `22_mF` is 0.022 F.
 *
 * @param[in] val literal digits.
 * @return Value<Capacitance> carrying val * 1e-3 farads (SI base).
 */
constexpr Value<Capacitance> operator""_mF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-3);
}

/**
 * @brief Microfarads literal: `10_uF` is 10e-6 F.
 *
 * @param[in] val literal digits.
 * @return Value<Capacitance> carrying val * 1e-6 farads (SI base).
 */
constexpr Value<Capacitance> operator""_uF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-6);
}

/**
 * @brief Nanofarads literal: `100_nF` is 1e-7 F.
 *
 * @param[in] val literal digits.
 * @return Value<Capacitance> carrying val * 1e-9 farads (SI base).
 */
constexpr Value<Capacitance> operator""_nF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-9);
}

/**
 * @brief Picofarads literal: `100_pF` is 1e-10 F.
 *
 * @param[in] val literal digits.
 * @return Value<Capacitance> carrying val * 1e-12 farads (SI base).
 */
constexpr Value<Capacitance> operator""_pF(long double val) {
  return Value<Capacitance>(static_cast<double>(val) * 1e-12);
}

} // namespace wengine::units::literals


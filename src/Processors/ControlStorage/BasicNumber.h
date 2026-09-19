#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sim36::processors::controlstorage {

// NuBasic's decimal floating-point value.  The guest encoding is an
// exponent/sign byte followed by four (SPREC) or eight (LPREC) base-100
// mantissa digits.  Arithmetic is deliberately implemented without host
// floating point.
class BasicNumber {
public:
    enum class Precision { Short, Long };
    enum class Condition { None, Overflow, Underflow, DivideByZero, IntegerRange };

    static constexpr std::size_t encodedSize(Precision precision)
    {
        return precision == Precision::Short ? 5 : 9;
    }
    static constexpr unsigned significantDigits(Precision precision)
    {
        return precision == Precision::Short ? 6 : 14;
    }

    static BasicNumber zero(Precision precision);
    static BasicNumber fromInteger(std::int64_t value, Precision precision, Condition& condition);
    static std::optional<BasicNumber> decode(const std::uint8_t* bytes, std::size_t size,
                                             Precision precision, std::string* error = nullptr);

    std::vector<std::uint8_t> encode() const;
    Precision precision() const { return precision_; }
    bool isZero() const;
    bool negative() const { return negative_ && !isZero(); }
    std::uint8_t exponent() const { return exponent_; }
    std::uint8_t digit(std::size_t index) const { return digits_.at(index); }

    int compare(const BasicNumber& other) const;
    // SLIC blstrnd conversion used by array-index operations.  Values are
    // rounded to nearest with a half away from zero.  Magnitudes whose
    // characteristic exceeds 0x42 return the signed 0x4000 sentinel and set
    // IntegerRange; callers still apply their own array bounds afterwards.
    std::int16_t roundedInteger(Condition& condition) const;
    BasicNumber negated() const;
    BasicNumber add(const BasicNumber& other, Condition& condition) const;
    BasicNumber subtract(const BasicNumber& other, Condition& condition) const;
    BasicNumber multiply(const BasicNumber& other, Condition& condition) const;
    BasicNumber divide(const BasicNumber& other, Condition& condition) const;

private:
    Precision precision_ = Precision::Short;
    bool negative_ = false;
    std::uint8_t exponent_ = 0;
    std::array<std::uint8_t, 8> digits_{};
};

}  // namespace sim36::processors::controlstorage

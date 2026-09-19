#include "Processors/ControlStorage/BasicNumber.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <boost/multiprecision/cpp_int.hpp>

namespace sim36::processors::controlstorage {
namespace {

using boost::multiprecision::cpp_int;

struct Rational {
    cpp_int numerator;
    cpp_int denominator{1};
};

cpp_int power10(unsigned power)
{
    cpp_int answer = 1;
    for (unsigned i = 0; i < power; ++i) answer *= 10;
    return answer;
}

cpp_int mantissa(const BasicNumber& number)
{
    cpp_int answer = 0;
    const std::size_t count = BasicNumber::encodedSize(number.precision()) - 1;
    for (std::size_t i = 0; i < count; ++i) answer = answer * 100 + number.digit(i);
    return answer;
}

Rational magnitude(const BasicNumber& number)
{
    const int pairs = static_cast<int>(BasicNumber::encodedSize(number.precision()) - 1);
    const int power = static_cast<int>(number.exponent()) - 64 - pairs;
    Rational answer{mantissa(number), 1};
    if (power >= 0)
        answer.numerator *= power10(static_cast<unsigned>(power * 2));
    else
        answer.denominator *= power10(static_cast<unsigned>(-power * 2));
    return answer;
}

int decimalOrder(const cpp_int& numerator, const cpp_int& denominator)
{
    // Find t such that 10^t <= numerator/denominator < 10^(t+1).
    const std::string n = numerator.convert_to<std::string>();
    const std::string d = denominator.convert_to<std::string>();
    int t = static_cast<int>(n.size()) - static_cast<int>(d.size());
    if (t >= 0) {
        if (numerator < denominator * power10(static_cast<unsigned>(t))) --t;
    } else if (numerator * power10(static_cast<unsigned>(-t)) < denominator) {
        --t;
    }
    return t;
}

BasicNumber quantize(cpp_int numerator, cpp_int denominator, bool negative,
                     BasicNumber::Precision precision, BasicNumber::Condition& condition)
{
    condition = BasicNumber::Condition::None;
    if (numerator == 0) return BasicNumber::zero(precision);
    if (numerator < 0) {
        numerator = -numerator;
        negative = !negative;
    }
    if (denominator < 0) {
        denominator = -denominator;
        negative = !negative;
    }

    int order = decimalOrder(numerator, denominator);
    const int precisionDigits = static_cast<int>(BasicNumber::significantDigits(precision));
    const int scale = precisionDigits - 1 - order;
    cpp_int scaledNumerator = numerator;
    cpp_int scaledDenominator = denominator;
    if (scale >= 0)
        scaledNumerator *= power10(static_cast<unsigned>(scale));
    else
        scaledDenominator *= power10(static_cast<unsigned>(-scale));

    cpp_int coefficient = scaledNumerator / scaledDenominator;
    const cpp_int remainder = scaledNumerator % scaledDenominator;
    // NuBasic rounds a discarded 5/50 upward in magnitude, including ties.
    if (remainder * 2 >= scaledDenominator) ++coefficient;
    const cpp_int limit = power10(static_cast<unsigned>(precisionDigits));
    if (coefficient == limit) {
        coefficient /= 10;
        ++order;
    }

    // The stored mantissa is a fraction.  q is its base-100 exponent and the
    // guest characteristic is q biased by 64.
    const int q = order >= 0 ? order / 2 + 1 : (order - 1) / 2 + 1;
    const int guestExponent = q + 64;
    if (guestExponent < 0) {
        condition = BasicNumber::Condition::Underflow;
        return BasicNumber::zero(precision);
    }

    std::vector<std::uint8_t> encoded(BasicNumber::encodedSize(precision), 0);
    if (guestExponent > 127) {
        condition = BasicNumber::Condition::Overflow;
        encoded[0] = 0x7F;
        std::fill(encoded.begin() + 1, encoded.end(), 99);
        return *BasicNumber::decode(encoded.data(), encoded.size(), precision);
    }

    cpp_int stored = coefficient * power10(static_cast<unsigned>(order + 3 - 2 * q));
    for (std::size_t i = encoded.size(); i-- > 1;) {
        encoded[i] = static_cast<std::uint8_t>((stored % 100).convert_to<unsigned>());
        stored /= 100;
    }
    encoded[0] = static_cast<std::uint8_t>(guestExponent | (negative ? 0x80 : 0));
    return *BasicNumber::decode(encoded.data(), encoded.size(), precision);
}

bool samePrecision(const BasicNumber& left, const BasicNumber& right)
{
    return left.precision() == right.precision();
}

}  // namespace

BasicNumber BasicNumber::zero(Precision precision)
{
    BasicNumber answer;
    answer.precision_ = precision;
    return answer;
}

BasicNumber BasicNumber::fromInteger(std::int64_t value, Precision precision, Condition& condition)
{
    const bool negative = value < 0;
    cpp_int magnitudeValue = value;
    if (magnitudeValue < 0) magnitudeValue = -magnitudeValue;
    return quantize(magnitudeValue, 1, negative, precision, condition);
}

std::optional<BasicNumber> BasicNumber::decode(const std::uint8_t* bytes, std::size_t size,
                                                Precision precision, std::string* error)
{
    if (error) error->clear();
    const std::size_t expected = encodedSize(precision);
    auto fail = [&](const char* message) -> std::optional<BasicNumber> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!bytes) return fail("null BASIC number");
    if (size != expected) return fail("wrong BASIC number width");

    BasicNumber answer;
    answer.precision_ = precision;
    answer.negative_ = (bytes[0] & 0x80) != 0;
    answer.exponent_ = bytes[0] & 0x7F;
    bool any = false;
    for (std::size_t i = 1; i < expected; ++i) {
        if (bytes[i] > 99) return fail("BASIC mantissa digit exceeds base 100");
        answer.digits_[i - 1] = bytes[i];
        any = any || bytes[i] != 0;
    }
    if (!any) {
        if (bytes[0] != 0) return fail("non-canonical BASIC zero");
        return answer;
    }
    if (answer.digits_[0] == 0) return fail("unnormalized BASIC mantissa");
    return answer;
}

std::vector<std::uint8_t> BasicNumber::encode() const
{
    std::vector<std::uint8_t> answer(encodedSize(precision_), 0);
    if (isZero()) return answer;
    answer[0] = static_cast<std::uint8_t>(exponent_ | (negative_ ? 0x80 : 0));
    std::copy_n(digits_.begin(), answer.size() - 1, answer.begin() + 1);
    return answer;
}

bool BasicNumber::isZero() const
{
    const std::size_t count = encodedSize(precision_) - 1;
    return std::all_of(digits_.begin(), digits_.begin() + count,
                       [](std::uint8_t digit) { return digit == 0; });
}

BasicNumber BasicNumber::negated() const
{
    BasicNumber answer = *this;
    if (!answer.isZero()) answer.negative_ = !answer.negative_;
    return answer;
}

int BasicNumber::compare(const BasicNumber& other) const
{
    if (!samePrecision(*this, other)) return precision_ == Precision::Short ? -1 : 1;
    if (isZero() && other.isZero()) return 0;
    if (negative() != other.negative()) return negative() ? -1 : 1;
    const Rational left = magnitude(*this);
    const Rational right = magnitude(other);
    const cpp_int l = left.numerator * right.denominator;
    const cpp_int r = right.numerator * left.denominator;
    int answer = l < r ? -1 : (l > r ? 1 : 0);
    return negative() ? -answer : answer;
}

std::int16_t BasicNumber::roundedInteger(Condition& condition) const
{
    condition = Condition::None;
    if (isZero() || exponent_ < 0x40) return 0;
    if (exponent_ > 0x42) {
        condition = Condition::IntegerRange;
        return negative() ? static_cast<std::int16_t>(-0x4000) : static_cast<std::int16_t>(0x4000);
    }

    unsigned magnitudeValue = 0;
    if (exponent_ == 0x40) {
        magnitudeValue = digits_[0] >= 50 ? 1U : 0U;
    } else if (exponent_ == 0x41) {
        magnitudeValue = digits_[0] + (digits_[1] >= 50 ? 1U : 0U);
    } else {
        magnitudeValue = digits_[0] * 100U + digits_[1] + (digits_[2] >= 50 ? 1U : 0U);
    }
    const int signedValue = negative() ? -static_cast<int>(magnitudeValue)
                                       : static_cast<int>(magnitudeValue);
    return static_cast<std::int16_t>(signedValue);
}

BasicNumber BasicNumber::add(const BasicNumber& other, Condition& condition) const
{
    if (!samePrecision(*this, other)) {
        condition = Condition::Underflow;
        return zero(precision_);
    }
    const Rational left = magnitude(*this);
    const Rational right = magnitude(other);
    cpp_int numerator = left.numerator * right.denominator;
    numerator *= negative() ? -1 : 1;
    cpp_int otherNumerator = right.numerator * left.denominator;
    otherNumerator *= other.negative() ? -1 : 1;
    numerator += otherNumerator;
    return quantize(numerator, left.denominator * right.denominator, false, precision_, condition);
}

BasicNumber BasicNumber::subtract(const BasicNumber& other, Condition& condition) const
{
    return add(other.negated(), condition);
}

BasicNumber BasicNumber::multiply(const BasicNumber& other, Condition& condition) const
{
    if (!samePrecision(*this, other)) {
        condition = Condition::Underflow;
        return zero(precision_);
    }
    const Rational left = magnitude(*this);
    const Rational right = magnitude(other);
    return quantize(left.numerator * right.numerator, left.denominator * right.denominator,
                    negative() != other.negative(), precision_, condition);
}

BasicNumber BasicNumber::divide(const BasicNumber& other, Condition& condition) const
{
    if (!samePrecision(*this, other) || other.isZero()) {
        condition = Condition::DivideByZero;
        return zero(precision_);
    }
    const Rational left = magnitude(*this);
    const Rational right = magnitude(other);
    return quantize(left.numerator * right.denominator, left.denominator * right.numerator,
                    negative() != other.negative(), precision_, condition);
}

}  // namespace sim36::processors::controlstorage

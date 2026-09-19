#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "Processors/ControlStorage/BasicNumber.h"

using sim36::processors::controlstorage::BasicNumber;

namespace {

BasicNumber number(std::initializer_list<std::uint8_t> bytes, BasicNumber::Precision precision)
{
    const std::vector<std::uint8_t> value(bytes);
    auto decoded = BasicNumber::decode(value.data(), value.size(), precision);
    REQUIRE(decoded.has_value());
    return *decoded;
}

}  // namespace

TEST_CASE("NuBasic decimal floating encodings are validated")
{
    using P = BasicNumber::Precision;
    CHECK(number({0x41, 1, 0, 0, 0}, P::Short).encode()
          == std::vector<std::uint8_t>{0x41, 1, 0, 0, 0});
    CHECK(number({0x40, 50, 0, 0, 0}, P::Short).encode()
          == std::vector<std::uint8_t>{0x40, 50, 0, 0, 0});
    CHECK(number({0xC1, 1, 0, 0, 0}, P::Short).negative());

    const std::array<std::uint8_t, 5> badDigit{0x41, 100, 0, 0, 0};
    CHECK_FALSE(BasicNumber::decode(badDigit.data(), badDigit.size(), P::Short));
    const std::array<std::uint8_t, 5> leadingZero{0x41, 0, 1, 0, 0};
    CHECK_FALSE(BasicNumber::decode(leadingZero.data(), leadingZero.size(), P::Short));
    const std::array<std::uint8_t, 5> negativeZero{0x80, 0, 0, 0, 0};
    CHECK_FALSE(BasicNumber::decode(negativeZero.data(), negativeZero.size(), P::Short));
}

TEST_CASE("NuBasic integer conversion and comparison use decimal values")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    const BasicNumber one = BasicNumber::fromInteger(1, P::Short, condition);
    CHECK(condition == C::None);
    CHECK(one.encode() == std::vector<std::uint8_t>{0x41, 1, 0, 0, 0});
    const BasicNumber hundred = BasicNumber::fromInteger(100, P::Short, condition);
    CHECK(hundred.encode() == std::vector<std::uint8_t>{0x42, 1, 0, 0, 0});
    const BasicNumber minusOne = BasicNumber::fromInteger(-1, P::Short, condition);
    CHECK(minusOne.encode() == std::vector<std::uint8_t>{0xC1, 1, 0, 0, 0});
    CHECK(minusOne.compare(one) < 0);
    CHECK(one.compare(hundred) < 0);
}

TEST_CASE("NuBasic SPREC rounds to six significant decimal digits half away from zero")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    const BasicNumber one = number({0x41, 1, 0, 0, 0}, P::Short);
    const BasicNumber belowHalf = number({0x3E, 4, 90, 0, 0}, P::Short); // 0.0000049
    const BasicNumber atHalf = number({0x3E, 5, 0, 0, 0}, P::Short);     // 0.0000050
    CHECK(one.add(belowHalf, condition).encode()
          == std::vector<std::uint8_t>{0x41, 1, 0, 0, 0});
    CHECK(one.add(atHalf, condition).encode()
          == std::vector<std::uint8_t>{0x41, 1, 0, 0, 10});
    CHECK(one.negated().subtract(atHalf, condition).encode()
          == std::vector<std::uint8_t>{0xC1, 1, 0, 0, 10});
}

TEST_CASE("NuBasic LPREC retains fourteen significant digits")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    const BasicNumber one = BasicNumber::fromInteger(1, P::Long, condition);
    const BasicNumber increment = number({0x3A, 5, 0, 0, 0, 0, 0, 0, 0}, P::Long);
    CHECK(one.add(increment, condition).encode()
          == std::vector<std::uint8_t>{0x41, 1, 0, 0, 0, 0, 0, 0, 10});
}

TEST_CASE("NuBasic arithmetic normalizes base-100 carries and cancellation")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    const BasicNumber one = BasicNumber::fromInteger(1, P::Short, condition);
    const BasicNumber two = BasicNumber::fromInteger(2, P::Short, condition);
    const BasicNumber three = one.add(two, condition);
    CHECK(three.encode() == std::vector<std::uint8_t>{0x41, 3, 0, 0, 0});
    CHECK(three.subtract(three, condition).isZero());

    const BasicNumber ninetyNine = BasicNumber::fromInteger(99, P::Short, condition);
    CHECK(ninetyNine.multiply(ninetyNine, condition).encode()
          == std::vector<std::uint8_t>{0x42, 98, 1, 0, 0});
    CHECK(three.divide(two, condition).encode()
          == std::vector<std::uint8_t>{0x41, 1, 50, 0, 0});
}

TEST_CASE("NuBasic arithmetic reports divide by zero overflow and underflow")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    const BasicNumber one = BasicNumber::fromInteger(1, P::Short, condition);
    const BasicNumber zero = BasicNumber::zero(P::Short);
    CHECK(one.divide(zero, condition).isZero());
    CHECK(condition == C::DivideByZero);

    const BasicNumber maximum = number({0x7F, 99, 99, 99, 0}, P::Short);
    CHECK(maximum.multiply(maximum, condition).encode()
          == std::vector<std::uint8_t>{0x7F, 99, 99, 99, 99});
    CHECK(condition == C::Overflow);

    const BasicNumber minimum = number({0x00, 1, 0, 0, 0}, P::Short);
    CHECK(minimum.multiply(minimum, condition).isZero());
    CHECK(condition == C::Underflow);
}

TEST_CASE("NuBasic blstrnd integer conversion rounds indices and returns its range sentinel")
{
    using C = BasicNumber::Condition;
    using P = BasicNumber::Precision;
    C condition{};
    CHECK(number({0x40, 49, 99, 0, 0}, P::Short).roundedInteger(condition) == 0);
    CHECK(condition == C::None);
    CHECK(number({0x40, 50, 0, 0, 0}, P::Short).roundedInteger(condition) == 1);
    CHECK(number({0xC0, 50, 0, 0, 0}, P::Short).roundedInteger(condition) == -1);
    CHECK(number({0x41, 49, 50, 0, 0}, P::Short).roundedInteger(condition) == 50);
    CHECK(number({0xC2, 99, 99, 50, 0}, P::Short).roundedInteger(condition) == -10000);
    CHECK(number({0x43, 1, 0, 0, 0}, P::Short).roundedInteger(condition) == 0x4000);
    CHECK(condition == C::IntegerRange);
    CHECK(number({0xC3, 1, 0, 0, 0}, P::Short).roundedInteger(condition) == -0x4000);
    CHECK(condition == C::IntegerRange);
}

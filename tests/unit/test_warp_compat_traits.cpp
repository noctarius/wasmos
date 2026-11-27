/* test_warp_compat_traits.cpp - what the WARP backend's freestanding <limits>,
 * <type_traits> and <functional> (src/kernel/warp/compat) answer.
 *
 * Built the way the kernel builds WARP: C++14, -nostdinc++, and the compat
 * directory on the include path, so <limits> below is the compat header and no
 * host C++ library is visible. Every trait answer is captured as a constexpr bool
 * and checked at run time, so a wrong answer is a failing case, not a build
 * break. */

#include <stdio.h>
#include <stdlib.h>

#include <functional>
#include <limits>
#include <type_traits>

#include "test_shuffle.h"

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

namespace {

template <typename...> struct make_void {
    using type = void;
};

/* has_max<T>: numeric_limits<T>::max() names a usable function. */
template <typename T, typename = void> struct has_max : std::false_type {};
template <typename T>
struct has_max<T, typename make_void<decltype(std::numeric_limits<T>::max())>::type>
    : std::true_type {};

/* is_callable<F, Args...>: an F lvalue can be called with Args. */
template <typename Void, typename F, typename... Args> struct is_callable_impl : std::false_type {};
template <typename F, typename... Args>
struct is_callable_impl<
    typename make_void<decltype(std::declval<F&>()(std::declval<Args>()...))>::type, F, Args...>
    : std::true_type {};
template <typename F, typename... Args> struct is_callable : is_callable_impl<void, F, Args...> {};

struct OnlyExplicit {
    explicit OnlyExplicit(int) {}
};

struct NotAnInt {};

enum class Colour : unsigned char { Red };

} // namespace

/* Regression: 2026-11-27-warp-compat-limits-zero -- the unspecialised
 * numeric_limits<T> answered max() == min() == 0 for every type without a
 * specialisation, with no diagnostic. A "rest of the span" sentinel or an
 * all-ones register mask built from max() silently became 0. */
static void test_a_type_without_limits_has_no_max(void) {
    constexpr bool long_double = has_max<long double>::value;
    constexpr bool enumeration = has_max<Colour>::value;
    constexpr bool pointer = has_max<int*>::value;
    CHECK(!long_double, "numeric_limits<long double>::max() exists and answers 0");
    CHECK(!enumeration, "numeric_limits<enum>::max() exists and answers 0");
    CHECK(!pointer, "numeric_limits<int*>::max() exists and answers 0");
}

/* The specialisations were keyed on the fixed-width macros (__UINT64_TYPE__ ...),
 * which name one of long and long long per target. The other one fell through to
 * the zero answer: on a target where size_t is unsigned long and uint64_t is
 * unsigned long long, numeric_limits<size_t>::max() was 0. */
static void test_every_integer_type_has_its_real_limits(void) {
    CHECK(std::numeric_limits<unsigned long>::max() == ~0ul, "unsigned long max");
    CHECK(std::numeric_limits<unsigned long long>::max() == ~0ull, "unsigned long long max");
    CHECK(std::numeric_limits<long>::max() == __LONG_MAX__, "long max");
    CHECK(std::numeric_limits<long>::min() == -__LONG_MAX__ - 1, "long min");
    CHECK(std::numeric_limits<long long>::max() == __LONG_LONG_MAX__, "long long max");
    CHECK(std::numeric_limits<size_t>::max() == __SIZE_MAX__, "size_t max");
    CHECK(std::numeric_limits<uint64_t>::max() == 0xFFFFFFFFFFFFFFFFull, "uint64_t max");
    CHECK(std::numeric_limits<uint32_t>::max() == 0xFFFFFFFFu, "uint32_t max");
    CHECK(std::numeric_limits<uint8_t>::max() == 0xFFu, "uint8_t max");
    CHECK(std::numeric_limits<int8_t>::min() == -128, "int8_t min");
    CHECK(std::numeric_limits<int32_t>::min() == -__INT_MAX__ - 1, "int32_t min");
    CHECK(std::numeric_limits<unsigned short>::max() == 0xFFFFu, "unsigned short max");
    CHECK(std::numeric_limits<char>::max() == __SCHAR_MAX__ ||
              std::numeric_limits<char>::max() == 0xFF,
          "char max");
    CHECK(std::numeric_limits<unsigned long>::is_integer, "unsigned long is_integer");
    CHECK(std::numeric_limits<double>::max() == __DBL_MAX__, "double max");
}

/* Regression: 2026-11-27-warp-compat-is-convertible -- is_convertible<From, To>
 * asked whether To is constructible from From, so a type reachable only through
 * an explicit constructor counted as implicitly convertible. */
static void test_is_convertible_rejects_explicit_only_conversions(void) {
    constexpr bool explicit_only = std::is_convertible<int, OnlyExplicit>::value;
    constexpr bool implicit = std::is_convertible<int, long>::value;
    constexpr bool to_void_pointer = std::is_convertible<OnlyExplicit*, const void*>::value;
    CHECK(!explicit_only, "int counts as convertible to a type with only an explicit constructor");
    CHECK(implicit, "int is not convertible to long");
    CHECK(to_void_pointer, "a pointer is not convertible to const void*");
}

/* Regression: 2026-11-27-warp-compat-function-any -- std::function's converting
 * constructor accepted any F, so FunctionRef's constructor constraint
 * (is_convertible<F&&, std::function<R(Args...)>>, libs/warp FunctionRef.hpp)
 * admitted a callable of any signature, and a non-callable as well. */
static void test_function_accepts_only_a_matching_callable(void) {
    auto takes_int = [](int) {};
    auto takes_struct = [](NotAnInt) {};
    auto returns_int = [](int v) { return v; };
    auto returns_struct = [](int) { return NotAnInt{}; };
    using VoidOfInt = std::function<void(int)>;
    using IntOfInt = std::function<int(int)>;
    constexpr bool matching = std::is_convertible<decltype(takes_int)&&, VoidOfInt>::value;
    constexpr bool discarded_result =
        std::is_convertible<decltype(returns_int)&&, VoidOfInt>::value;
    constexpr bool wrong_argument = std::is_convertible<decltype(takes_struct)&&, VoidOfInt>::value;
    constexpr bool wrong_result = std::is_convertible<decltype(returns_struct)&&, IntOfInt>::value;
    constexpr bool not_callable = std::is_convertible<NotAnInt&&, VoidOfInt>::value;
    constexpr bool null = std::is_convertible<decltype(nullptr), VoidOfInt>::value;
    CHECK(matching, "a matching lambda is not convertible to std::function");
    CHECK(discarded_result, "a lambda whose result a void function discards is refused");
    CHECK(!wrong_argument, "a lambda with the wrong argument type is convertible");
    CHECK(!wrong_result, "a lambda with an unconvertible result is convertible");
    CHECK(!not_callable, "a non-callable object is convertible to std::function");
    CHECK(!null, "nullptr selects the converting constructor");
}

/* Regression: 2026-11-27-warp-compat-function-spin -- invoking a std::function
 * looped forever, because the wrapper stores no callable to dispatch to. */
static void test_a_function_cannot_be_invoked(void) {
    constexpr bool callable = is_callable<std::function<void(int)>, int>::value;
    constexpr bool const_callable = is_callable<const std::function<int()>>::value;
    CHECK(!callable, "std::function::operator() is callable and never returns");
    CHECK(!const_callable, "const std::function::operator() is callable and never returns");
}

int main(void) {
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_a_type_without_limits_has_no_max),
        WASMOS_TEST_CASE(test_every_integer_type_has_its_real_limits),
        WASMOS_TEST_CASE(test_is_convertible_rejects_explicit_only_conversions),
        WASMOS_TEST_CASE(test_function_accepts_only_a_matching_callable),
        WASMOS_TEST_CASE(test_a_function_cannot_be_invoked),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_warp_compat_traits: ok\n");
    return 0;
}

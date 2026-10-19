// Assertion-policy pin: every CTest binary links wengine_test_assert
// (-UNDEBUG), so NDEBUG must never be defined here. The #error makes
// an accidentally disabled-assert build fail at compile time; the
// runtime side-effect check makes it fail loudly even if the guard
// above were ever removed.
#ifdef NDEBUG
#error "WEngine test policy: NDEBUG must not be defined for test targets (would disable assert). Ensure the target links wengine_test_assert."
#endif

#include <cassert>
#include <iostream>

int main() {
  // Side-effect check: assert() must execute its expression. Under
  // NDEBUG this assignment would be skipped and the explicit check
  // below fails instead of silently passing.
  bool side_effect_ran = false;
  assert((side_effect_ran = true, true));
  if (!side_effect_ran) {
    std::cout << "test_assert_policy: FAIL (assert side effect skipped, NDEBUG active?)\n";
    return 1;
  }

  // Sanity: a true assertion passes.
  assert(1 + 1 == 2);

  std::cout << "test_assert_policy: OK\n";
  return 0;
}

// Intentional-failure probe: assert(false) must abort, even in an
// optimized build. Registered with WILL_FAIL, so an accidentally
// disabled assert (clean exit 0) is reported as an unexpected pass
// instead of silently succeeding.
#ifdef NDEBUG
#error "WEngine test policy: NDEBUG must not be defined for test targets (would disable assert). Ensure the target links wengine_test_assert."
#endif

#include <cassert>

int main() {
  assert(false && "wengine assert probe: intentional failure");
  return 0;
}

# Portability gate (US2, T011)

- Default build (`build/`, dev flags): full `cmake --build build`
  completes with **zero warnings** (`grep -ci warning` on the build
  log → 0).
- Bench preset (`build-bench`, Release `-O3 -DNDEBUG`): `bench_logic`
  target builds with **zero warnings/errors**.
- Intrinsics/target-attribute gate: `grep` for
  `immintrin|arm_neon|__m128|__m256|__m512|target(|march=` over all
  new/changed files (`eval.hpp`, `block_interface.hpp`, `types.hpp`,
  `test_logic_word_batch.cpp`, `bench_logic.cpp`, `common.cpp`) →
  **no matches**. Only hits in the repo are vendored `external/glm`
  (explicitly out of scope).
- New dependencies: **none**. Engine changes are header-only additions
  using existing includes (`<cstddef>`, `<cstdint>`, `<span>`,
  `<vector>`); no new `#include`, no new TU, no new link edge.
- Compiler pin (factual, host-provided): bench preset echoes
  `CXX=GNU 16.2.1 FLAGS=' -O3 -DNDEBUG'` (recorded in
  `build-bench/CMakeCache.txt`); baseline discipline remains GCC 13.3
  but the host toolchain is what measured the numbers.
- Fallback identity: the single-slice path is byte-untouched (no diff
  in `eval_block_word`); tail/single composition proven by
  `test_batch_then_tail` + `--limit 400` CLI byte-identity (1 batch +
  3 tail words).

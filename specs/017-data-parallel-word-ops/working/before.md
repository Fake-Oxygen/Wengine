# Before-state (pre-017)

- HEAD: `feca3c14e2ada8180be43a86d7ee88edb91e9a9e`
- Baseline suite: `ctest --test-dir build` → 82/82 pass (44.96s), zero pre-existing failures.
- Before-number (`build-bench/bench_logic`, default 16w x 16d x 20000 words):
  `bench_logic: 256 gates (16w x 16d) x 20000 words, 18.1062 ns/gate-word (checksum=8274861028103430378)`
- Env pin: GCC 16.2.1, `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG` (from
  `build-bench/CMakeCache.txt`, echoed at bench configure), Linux host.
- NOTE (pre-existing defect, found during T002): `bench_logic` aborts
  AFTER printing the word line — the trailing shared-solve section
  reads `diag.per_net[f]` for `f < kMaxGates*kMaxInputs` (8M) while
  `solve()` sizes `per_net` to the pool (`n*kMaxInputs`); Release
  segfaults (exit 139), dev build trips the STL bounds assert.
  Bench file is pristine at HEAD (`git status` clean), so the defect
  predates this feature (015 dynamic-pool migration left the bench
  on the old fixed-capacity assumption). The word-section number
  above is unaffected (prints before the crash; identical checksum
  in dev and Release runs). A minimal bound fix is planned alongside
  the T009 batch-section edit so US4 can run the full sweep.

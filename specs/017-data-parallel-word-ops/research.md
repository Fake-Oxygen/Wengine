# Research: Data-Parallel Word Ops for Bit-Parallel Evaluator

**Date**: 2026-11-22 | **Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

All unknowns resolved below. No NEEDS CLARIFICATION remains.

## R1 — Parallelism direction: slice-blocking across consecutive words

**Decision**: Evaluate a batch of `W` consecutive 64-row slices in one
level-order traversal (slice-blocking). Each gate computes `W` words
instead of one; `resolve` runs once per gate-input per batch instead
of once per gate-input per slice. Lane width stays 64; the row/lane
convention (`lane b holds row w*64+b`) is untouched.

**Rationale**: The measured hot loop (`eval_block_word`,
`eval.hpp:262-382`) repeats three word-independent costs per slice:
the level-order walk itself, `build_input_positions`-style lookups,
and — dominant — `assemble_input_span` per gate-input per slice
(`input_resolution.hpp:99-160`: branches over marks, extras, ties,
pulls, all word-independent). Slice-blocking amortizes all three
over `W` slices while every emitted word keeps the exact
`input_column_word(p, w)` value for its own `w`. Lane-widening
(128/256-bit lanes) would instead redefine the row convention,
`kBitWordSize`, masking, and every caller — a semantic change the
spec forbids.

**Alternatives considered**:
- *Wider lanes (e.g. 128-bit truth words)*: rejected — changes
  `kBitWordSize`, the `truth` verb's row mapping, masking
  (`eval.hpp:374-380`), and `bit_words_for_inputs` contract surface.
- *Threading across slices*: rejected — hot engine paths forbid
  unnecessary synchronization (AGENTS.md §5); spawning cost dwarfs
  a gate-op batch, and determinism review surface explodes.
- *GPU offload*: rejected — same plus transport/bridge policy
  (delivery classes, edge-only ISinks) makes in-evaluator offload
  an architectural violation, not an optimization.

## R2 — No intrinsics: scalar batch loop + compiler auto-vectorization

**Decision**: The batch inner loop is plain scalar `uint64_t` ops
(`&`, `|`, `^`, `~`) over a small fixed batch — the same branchless
word ops as today, repeated `W` times. Speedup comes from (a) doing
span assembly and column derivation once per batch, and (b) the
`-O3` optimizer auto-vectorizing the straight-line batch loop. No
`<immintrin.h>` / NEON includes, no `target` attributes, no
`-march=` flags, no CPUID dispatch.

**Rationale**: Four binding constraints converge here:
1. Engine headers stay platform-free (AGENTS.md §4 — a Windows port
   replaces TUs, never headers; arch intrinsics in a header would
   leak platform knowledge into the engine).
2. Zero-warning baseline on GCC 13.3 with `-Wconversion` etc.:
   intrinsic types (`__m256i`) fight `-Wconversion`-clean generic
   code and poison `constexpr` contexts (`input_column_word` is
   `constexpr`; SIMD intrinsics are not constant expressions — cf.
   the GLM `GLM_FORCE_PURE` precedent noted in-repo).
3. The repo pins **no** `-march` flags anywhere in its own targets
   (only vendored GLM options, all `OFF`); adding one would fork
   the build matrix and the bench environment pin.
4. The gate-op mix is the friendliest possible auto-vectorization
   target: fixed-trip-count, aliasing-free, branchless, pure
   bitwise ops. The measured win does not require hand-vectorizing.

The "portable fallback" (spec US2) is therefore not a second code
path behind a macro — it is the *same* batch source compiled
anywhere, plus the untouched single-slice `eval_block_word` kept as
the correctness reference and the tail path. Availability never
changes semantics because there is only one semantics.

**Alternatives considered**:
- *Explicit AVX2/NEON intrinsics with runtime dispatch*: rejected —
  violates header platform-freedom, needs CPUID + two code paths to
  keep bit-identical, doubles the parity surface, and adds
  `-Wconversion` friction for unmeasured extra gain.
- *`[[gnu::target("avx2")]]` multi-versioning*: rejected — GCC-only
  attribute in engine headers breaks the platform-free rule and the
  baseline-compiler portability story; same dual-path cost.
- *GCC vector extensions (`vector_size`)*: rejected — milder but
  still a compiler-specific dialect in engine headers with
  alignment/`-Wconversion` sharp edges; revisit only if the
  benchmark shows auto-vectorization failed (the plan's measurement
  step checks the generated code).

## R3 — Batch width: fixed `kWordBatchWidth = 4`

**Decision**: `W = 4` consecutive slices per batched pass (256 truth
rows per traversal), fixed at compile time as
`logic::kWordBatchWidth`. The tail (`count % 4 != 0`) evaluates
through the untouched single-slice path.

**Rationale**: 4 is the smallest width that (a) fills one AVX2
register of `uint64_t` lanes when auto-vectorized (4×64-bit ops per
instruction — the "SIMD multiplies 64-wide parallelism" of the task
title, delivered through the compiler), (b) keeps batch scratch
small (`n_slots × 4` words — 32 KiB at the 1024-slot scale; scales
with the pool exactly like `ws_words`), and (c) keeps the tail path
trivially correct (single-slice loop, zero new masking logic — the
standing partial-word mask in `eval.hpp:374-380` applies per word
unchanged). Larger widths (8/16) multiply scratch and the
`input_column_word` staging with diminishing amortization returns,
since span assembly — the dominant saved cost — is already fully
amortized at any `W > 1`.

**Alternatives considered**:
- *W=8/16*: rejected — 2×/4× scratch for sub-linear extra gain;
  revisit on measurement.
- *Runtime-tunable width*: rejected — a tuning knob with no
  measured need; callers would need width discovery + variable
  scratch. Fixed width keeps scratch formulas closed-form.

## R4 — Scratch layout: interleaved `[slot][w]`, pool workspace extended

**Decision**: Batch scratch is `n_slots × W` words, slot-major:
`base[slot*W + k]` holds gate `slot`'s word for batch lane `k`
(slice `word_base + k`). Contiguous `W` words per gate make the
per-gate inner loop stride-1 (auto-vectorizer-friendly) and keep the
`resolve` read (`gate_scratch[drv*W + k]`) equally stride-1. Sizing
formula `batch_words(n) = n * kWordBatchWidth` lives beside
`bit_words_for_inputs` in `block_interface.hpp`. The pool gains
`ws_wordblock` (amortized growth via `ensure_workspace`, same
convention as `ws_words`); the batched entry keeps the caller-owned
span overload as primary (steady-state allocation-free either way),
with a pool-backed convenience overload mirroring `solve`'s two
overloads (`eval.hpp:222-230` precedent).

**Rationale**: Slot-major matches access order (finish all of one
gate's `W` words before the next gate — the level order demands the
whole word before successors read it). Word-major (`[w][slot]`)
would stride by `W` on every gate write. Pool-workspace extension
follows the 015/016 standing convention (workspace grows amortized,
consumers refuse loudly on undersize).

**Alternatives considered**:
- *Word-major `[w][slot]`*: rejected — strided gate writes,
  hostile to auto-vectorization.
- *Caller-only scratch (no pool workspace)*: rejected — every
  existing caller uses pool-backed convenience; forcing all
  callers to size `n*W` spans by hand repeats the pre-015 scratch
  ergonomics the codebase already centralized.

## R5 — Guards fire before batch work; TBUF/sequential arms preserved

**Decision**: The batched entry performs the identical entry checks
as `eval_block_word` (topology compile, scratch/output sizes,
`word_base + W` range validation for `n_inputs < 31`) *before* any
batch loop, and the word-loop entry guards (`shared_word_blocked`,
`has_buffer_gates` in `common.cpp:331/412` and bench) keep refusing
four-value nets before either path runs. Inside the batch walk, the
`kTbuf` arm returns `kInvalidArgument` exactly as today
(`eval.hpp:343-349` safety net), and sequential kinds contribute
zero words per lane (`eval.hpp:350-357` fallthrough, lanes stay 0).
Partial-word masking applies per emitted word with the standing
formula — for `n_inputs >= 6` no mask, else mask only words whose
rows exceed `1<<n_inputs` (in a batch only the final word of the
full truth can be partial, and only when the batch straddles it;
per-word masking handles both cases uniformly).

**Rationale**: Guard-before-work is what makes the accelerated path
observably identical on refusal classes (spec FR-005); per-word
masking reuses the exact standing expression so don't-care lanes
can never leak through an unmasked batch lane.

## R6 — Caller adoption: batch main loop, single-slice tail

**Decision**: The two `eval_block_word` loops in
`src/cli/commands/logic/common.cpp:374/489` (truth streaming +
pretty table) and the timed loop in `benchmarks/bench_logic.cpp`
become `for (base step W) eval_block_words(base, W)` + `for
(rem) eval_block_word(...)` tails. `eval_block_word` itself stays
byte-for-byte (the portable reference + tail engine). The optimizer
exact/QM equivalence checks keep calling the single-slice entry —
they compare against scalar solve on small nets where batching buys
nothing and churn risks most.

**Rationale**: Minimal caller diff (loop shape only, no output
logic touched); the highest-volume loops (truth streaming,
benchmark) are exactly the measured hot path. Leaving the
equivalence checks on the single path keeps the independent
reference independent — the parity test (not the optimizer) is
where batch-vs-single agreement is proven.

## R7 — Measurement: bench preset, opaque inputs, committed deltas

**Decision**: Before-number is the committed 015/016
`bench_logic` per-gate-word figure on the pinned bench preset
(`-O3 -DNDEBUG`, opaque xorshift construction, checksummed
outputs); after-number comes from the same binary + a new batch
section in the same binary (same net, same slices) plus the
adopted truth-loop timing. Both land in `docs/benchmarks/logic-scale.md`
beside the environment pin (compiler, flags, host), with the
stated repetition/variance policy; the full sweep re-run proves no
other stage regressed. Additionally the implementer inspects the
generated code for the batch inner loop (auto-vectorization
actually fired) before claiming the win — appearance-based
"it should vectorize" is not evidence.

**Alternatives considered**:
- *Separate batch benchmark binary*: rejected — same-net
  same-slice comparability is stronger inside one binary; a second
  binary doubles the bench-catalog audit surface
  (`docs/audit_catalogs.py` zero-missing/zero-ghost rule).

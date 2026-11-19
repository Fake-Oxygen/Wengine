#pragma once

/**
 * @file
 * Closed deterministic expression source (SIG-4a): a small fixed math
 * language for custom waveforms without a scripting dependency. Owns the
 * RPN opcode vocabulary, the config-edge parser, and pure fallible
 * evaluation. Depends only on core/status; never on world, emit, or Lua.
 * Consumed by the signal solver for kExpression sources.
 */
// Closed deterministic expression source (SIG-4a): a small fixed math
// language for custom waveforms without a scripting dependency.
//
// Language (intentionally closed — auditable, no growth without review):
//   literals: decimal/scientific via from_chars (2, 0.5, 1e-3; no hex,
//             no suffixes, no implicit multiplication: "2t" is an error)
//   variable: t — source seconds since start (same time base as every
//             other generator: value(t_end - t0), rebased on start)
//   constants: pi, e
//   binary: + - * / ^ (power, right-associative)
//   unary: - (negation; unary plus is rejected — write the literal)
//   functions: sin cos exp log sqrt abs (1 arg), min max (2 args)
//   grouping: parentheses; ASCII whitespace ignored.
//
// Parsing (`parse_expression`, config edge only) compiles text to a fixed
// RPN program (`ExprProgram`, <= kMaxExprNodes nodes, no allocation, no
// strings): identical text always yields the identical program, and the
// evaluator runs nodes in fixed program order, so results are exactly
// reproducible for fixed inputs (same libm caveat class as the parametric
// sine — no new determinism hazard).
//
// Errors are loud `Status`/`Result`, never NaN/inf leaks: division by an
// exactly-zero denominator, log(x <= 0), sqrt(x < 0), and any non-finite
// intermediate (overflow, out-of-domain pow) fail evaluation, and the
// solver maps that to a failed step (time/index frozen, nothing emitted)
// exactly like a dangling sum operand.

#include "wengine/core/status.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::signal {

// RPN opcodes. APPEND-ONLY for the same reason as SignalKind: programs
// persist in the pool and the v4 state blob, so code bytes stay stable.
/**
 * @brief RPN opcodes for closed expression programs.
 * @note APPEND-ONLY like SignalKind: programs persist in the pool and the
 * v4 state blob, so code bytes stay stable; kCount is a sentinel, never emitted.
 */
enum class ExprOp : std::uint8_t {
/** @brief Push node.value. */
  kConst = 0, // push node.value
/** @brief Push source time t. */
  kT = 1, // push t
/** @brief Pop b,a; push a + b. */
  kAdd = 2,
/** @brief Pop b,a; push a - b. */
  kSub = 3,
/** @brief Pop b,a; push a * b. */
  kMul = 4,
/** @brief Pop b,a; push a / b (exactly-zero denominator fails loudly). */
  kDiv = 5,
/** @brief Pop b,a; push pow(a, b) (right-associative in the grammar). */
  kPow = 6,
/** @brief Negate the top of stack. */
  kNeg = 7,
/** @brief Push sin(x). */
  kSin = 8,
/** @brief Push cos(x). */
  kCos = 9,
/** @brief Push exp(x). */
  kExp = 10,
/** @brief Push log(x); x <= 0 fails loudly. */
  kLog = 11,
/** @brief Push sqrt(x); x < 0 fails loudly. */
  kSqrt = 12,
/** @brief Push fabs(x). */
  kAbs = 13,
/** @brief Pop b,a; push min(a, b). */
  kMin = 14,
/** @brief Pop b,a; push max(a, b). */
  kMax = 15,
/** @brief Sentinel: valid codes are < kCount, never emitted. */
  kCount = 16, // sentinel: valid codes are < kCount, never emitted
};

/** @brief Maximum RPN nodes per expression program (fixed, no allocation). */
inline constexpr std::size_t kMaxExprNodes = 64;
/** @brief Maximum parenthesis/function nesting depth while parsing. */
inline constexpr std::size_t kMaxExprDepth = 32;

/**
 * @brief One RPN instruction: an opcode plus the kConst payload.
 * @note Fixed size, so per-step program copies stay memcpy.
 */
struct ExprNode {
/** @brief Opcode. */
  ExprOp op = ExprOp::kConst;
/** @brief Literal payload for kConst only. */
  double value = 0.0; // kConst payload only

/**
 * @brief Field-wise equality.
 * @return true when opcode and payload compare equal.
 */
  bool operator==(const ExprNode &) const noexcept = default;
};

/**
 * @brief Compiled expression: fixed RPN nodes evaluated in program order.
 * @note Identical text always compiles to the identical program, so results
 * are exactly reproducible for fixed inputs.
 */
struct ExprProgram {
/** @brief RPN nodes in evaluation order. */
  std::array<ExprNode, kMaxExprNodes> nodes{};
/** @brief Number of live nodes in nodes. */
  std::uint8_t size = 0;

/**
 * @brief Field-wise equality.
 * @return true when every live node compares equal.
 */
  bool operator==(const ExprProgram &) const noexcept = default;
};

namespace expr_detail {

/** @brief pi literal for the expression language. */
inline constexpr double kPi = 3.141592653589793;
/** @brief e literal for the expression language. */
inline constexpr double kE = 2.718281828459045;

/**
 * @brief Recursive-descent parser with precedence climbing; emits RPN directly.
 * @note Config edge only: the hot path never parses (see parse_expression).
 */
struct Parser {
/** @brief Borrowed expression text being parsed. */
  std::string_view in;
/** @brief Current scan position in in. */
  std::size_t pos = 0;
/** @brief Program under construction. */
  ExprProgram prog;
/** @brief Current nesting depth (bounded by kMaxExprDepth). */
  std::size_t depth = 0;

/**
 * @brief Advance pos past ASCII whitespace.
 * @note Whitespace is ignored everywhere in the language.
 */
  void skip_ws() noexcept {
    while (pos < in.size() &&
           (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' || in[pos] == '\r')) {
      ++pos;
    }
  }

/**
 * @brief Append one node to prog.
 * @param[in] op opcode to emit.
 * @param[in] value literal payload for kConst.
 * @return true on success, false when the program would exceed kMaxExprNodes.
 */
  [[nodiscard]] bool emit(ExprOp op, double value = 0.0) noexcept {
    if (prog.size >= kMaxExprNodes) {
      return false;
    }
    prog.nodes[prog.size] = ExprNode{op, value};
    ++prog.size;
    return true;
  }

  // Recursive descent with precedence climbing; emits RPN directly.
  // expr := term (('+'|'-') term)*
/**
 * @brief Parse expr := term (('+'|'-') term)*.
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_expr() {
    if (const char *e = parse_term()) {
      return e;
    }
    while (true) {
      skip_ws();
      if (pos < in.size() && (in[pos] == '+' || in[pos] == '-')) {
        const char op = in[pos++];
        if (const char *e = parse_term()) {
          return e;
        }
        if (!emit(op == '+' ? ExprOp::kAdd : ExprOp::kSub)) {
          return "expression too complex";
        }
        continue;
      }
      return nullptr;
    }
  }

  // term := unary (('*'|'/') unary)*
/**
 * @brief Parse term := unary (('*'|'/') unary)*.
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_term() {
    if (const char *e = parse_unary()) {
      return e;
    }
    while (true) {
      skip_ws();
      if (pos < in.size() && (in[pos] == '*' || in[pos] == '/')) {
        const char op = in[pos++];
        if (const char *e = parse_unary()) {
          return e;
        }
        if (!emit(op == '*' ? ExprOp::kMul : ExprOp::kDiv)) {
          return "expression too complex";
        }
        continue;
      }
      return nullptr;
    }
  }

  // unary := '-' unary | power   (unary plus rejected: write the literal)
/**
 * @brief Parse unary := '-' unary | power (unary plus is rejected: write the literal).
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_unary() {
    skip_ws();
    if (pos < in.size() && in[pos] == '-') {
      ++pos;
      if (const char *e = parse_unary()) {
        return e;
      }
      if (!emit(ExprOp::kNeg)) {
        return "expression too complex";
      }
      return nullptr;
    }
    return parse_power();
  }

  // power := primary ('^' unary)?   (right-associative, exponent binds unary)
/**
 * @brief Parse power := primary ('^' unary)? (right-associative, exponent binds unary).
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_power() {
    if (const char *e = parse_primary()) {
      return e;
    }
    skip_ws();
    if (pos < in.size() && in[pos] == '^') {
      ++pos;
      if (const char *e = parse_unary()) {
        return e;
      }
      if (!emit(ExprOp::kPow)) {
        return "expression too complex";
      }
    }
    return nullptr;
  }

/**
 * @brief Parse a parenthesized group, number, or name.
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_primary() {
    skip_ws();
    if (depth >= kMaxExprDepth) {
      return "expression too deeply nested";
    }
    if (pos >= in.size()) {
      return "unexpected end of expression";
    }
    const char c = in[pos];
    if (c == '(') {
      ++pos;
      ++depth;
      if (const char *e = parse_expr()) {
        return e;
      }
      skip_ws();
      if (pos >= in.size() || in[pos] != ')') {
        return "missing ')'";
      }
      ++pos;
      --depth;
      return nullptr;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
      return parse_number();
    }
    if (c >= 'a' && c <= 'z') {
      return parse_named();
    }
    return "unexpected character in expression";
  }

/**
 * @brief Parse a decimal/scientific literal via from_chars (no hex, no suffixes, no implicit multiplication).
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_number() {
    const char *beg = in.data() + pos;
    const char *end = in.data() + in.size();
    double v = 0.0;
    const auto res = std::from_chars(beg, end, v);
    if (res.ec != std::errc{} || res.ptr == beg) {
      return "invalid number in expression";
    }
    pos = static_cast<std::size_t>(res.ptr - in.data());
    // No implicit multiplication: "2t" and "2foo" are errors, not 2*t.
    if (pos < in.size() && ((in[pos] >= 'a' && in[pos] <= 'z') ||
                            (in[pos] >= 'A' && in[pos] <= 'Z') || in[pos] == '_')) {
      return "unexpected name after number (write '*' explicitly)";
    }
    if (!std::isfinite(v)) {
      return "non-finite literal in expression";
    }
    if (!emit(ExprOp::kConst, v)) {
      return "expression too complex";
    }
    return nullptr;
  }

/**
 * @brief Parse t, pi, e, or a function call (1-arg math, 2-arg min/max).
 * @return nullptr on success, or a literal diagnostic on failure.
 */
  [[nodiscard]] const char *parse_named() {
    std::size_t beg = pos;
    while (pos < in.size() && in[pos] >= 'a' && in[pos] <= 'z') {
      ++pos;
    }
    const std::string_view name = in.substr(beg, pos - beg);
    if (name == "t") {
      if (!emit(ExprOp::kT)) {
        return "expression too complex";
      }
      return nullptr;
    }
    if (name == "pi") {
      if (!emit(ExprOp::kConst, kPi)) {
        return "expression too complex";
      }
      return nullptr;
    }
    if (name == "e") {
      if (!emit(ExprOp::kConst, kE)) {
        return "expression too complex";
      }
      return nullptr;
    }
    // Single-arg functions.
    ExprOp fn = ExprOp::kCount;
    bool two_arg = false;
    if (name == "sin") {
      fn = ExprOp::kSin;
    } else if (name == "cos") {
      fn = ExprOp::kCos;
    } else if (name == "exp") {
      fn = ExprOp::kExp;
    } else if (name == "log") {
      fn = ExprOp::kLog;
    } else if (name == "sqrt") {
      fn = ExprOp::kSqrt;
    } else if (name == "abs") {
      fn = ExprOp::kAbs;
    } else if (name == "min") {
      fn = ExprOp::kMin;
      two_arg = true;
    } else if (name == "max") {
      fn = ExprOp::kMax;
      two_arg = true;
    } else {
      return "unknown name in expression";
    }
    skip_ws();
    if (pos >= in.size() || in[pos] != '(') {
      return "missing '(' after function name";
    }
    ++pos;
    ++depth;
    if (depth >= kMaxExprDepth) {
      return "expression too deeply nested";
    }
    if (const char *e = parse_expr()) {
      return e;
    }
    if (two_arg) {
      skip_ws();
      if (pos >= in.size() || in[pos] != ',') {
        return "missing ',' in two-argument function";
      }
      ++pos;
      if (const char *e = parse_expr()) {
        return e;
      }
    }
    skip_ws();
    if (pos >= in.size() || in[pos] != ')') {
      return "missing ')' after function call";
    }
    ++pos;
    --depth;
    if (!emit(fn)) {
      return "expression too complex";
    }
    return nullptr;
  }
};

} // namespace expr_detail

// Compile expression text to an RPN program (config edge only; hot path
// never parses). Empty/whitespace-only text is an error.
[[nodiscard]] inline core::Result<ExprProgram>
/**
 * @brief Compile expression text to a fixed RPN program (config edge only).
 * @param[in] text expression source; empty/whitespace-only text is an error.
 * @return Success with the compiled program, or kInvalidArgument with a literal reason.
 * @note Identical text always yields the identical program.
 */
parse_expression(std::string_view text) noexcept {
  expr_detail::Parser p;
  p.in = text;
  p.skip_ws();
  if (p.pos >= p.in.size()) {
    return core::Result<ExprProgram>::failure("empty expression");
  }
  if (const char *e = p.parse_expr()) {
    return core::Result<ExprProgram>::failure(e);
  }
  p.skip_ws();
  if (p.pos < p.in.size()) {
    return core::Result<ExprProgram>::failure("unexpected trailing text in expression");
  }
  if (p.prog.size == 0) {
    return core::Result<ExprProgram>::failure("empty expression");
  }
  return core::Result<ExprProgram>::success(p.prog);
}

// Structural validation for stored programs (pool contents, loaded blobs):
// codes in range, stack balanced, exactly one value left. Pure, noexcept.
/**
 * @brief Structural validation for stored programs (pool contents, loaded blobs).
 * @param[in] prog program to gate (codes in range, stack balanced, exactly one value left).
 * @return true when the program is structurally sound.
 * @note Pure and noexcept; evaluation still fails loudly on domain errors.
 */
[[nodiscard]] inline bool validate_program(const ExprProgram &prog) noexcept {
  if (prog.size == 0 || prog.size > kMaxExprNodes) {
    return false;
  }
  int depth = 0;
  for (std::uint8_t k = 0; k < prog.size; ++k) {
    const ExprOp op = prog.nodes[k].op;
    if (static_cast<std::uint8_t>(op) >= static_cast<std::uint8_t>(ExprOp::kCount)) {
      return false;
    }
    switch (op) {
    case ExprOp::kConst:
    case ExprOp::kT:
      if (!std::isfinite(prog.nodes[k].value) && op == ExprOp::kConst) {
        return false;
      }
      ++depth;
      break;
    case ExprOp::kAdd:
    case ExprOp::kSub:
    case ExprOp::kMul:
    case ExprOp::kDiv:
    case ExprOp::kPow:
    case ExprOp::kMin:
    case ExprOp::kMax:
      if (depth < 2) {
        return false;
      }
      --depth; // pop 2, push 1
      break;
    case ExprOp::kNeg:
    case ExprOp::kSin:
    case ExprOp::kCos:
    case ExprOp::kExp:
    case ExprOp::kLog:
    case ExprOp::kSqrt:
    case ExprOp::kAbs:
      if (depth < 1) {
        return false;
      }
      break; // pop 1, push 1: depth unchanged
    case ExprOp::kCount:
      return false;
    }
    if (depth > static_cast<int>(kMaxExprNodes)) {
      return false;
    }
  }
  return depth == 1;
}

// Evaluate a validated program at source time `t`. Failure (division by
// exactly-zero denominator, log(x <= 0), sqrt(x < 0), any non-finite
// intermediate) is a loud error — never a NaN/inf sample.
/**
 * @brief Evaluate a validated program at source time t.
 * @param[in] prog structurally valid program (see validate_program).
 * @param[in] t source seconds since start.
 * @return Success with the value, or kInvalidArgument for an exactly-zero divisor,
 * log(x <= 0), sqrt(x < 0), any non-finite intermediate, or a malformed program;
 * never a NaN/inf sample.
 * @note Failure maps to a failed step (time/index frozen, nothing emitted), like a dangling sum operand.
 */
[[nodiscard]] inline core::Result<double> eval_expression(const ExprProgram &prog,
                                                         double t) noexcept {
  double stack[kMaxExprNodes];
  std::size_t sp = 0;
  auto push = [&](double v) -> bool {
    if (sp >= kMaxExprNodes) {
      return false;
    }
    stack[sp++] = v;
    return true;
  };
  for (std::uint8_t k = 0; k < prog.size; ++k) {
    const ExprNode &n = prog.nodes[k];
    switch (n.op) {
    case ExprOp::kConst:
      if (!push(n.value)) {
        return core::Result<double>::failure("expression stack overflow");
      }
      break;
    case ExprOp::kT:
      if (!push(t)) {
        return core::Result<double>::failure("expression stack overflow");
      }
      break;
    case ExprOp::kNeg:
    case ExprOp::kSin:
    case ExprOp::kCos:
    case ExprOp::kExp:
    case ExprOp::kLog:
    case ExprOp::kSqrt:
    case ExprOp::kAbs: {
      if (sp < 1) {
        return core::Result<double>::failure("invalid expression program");
      }
      const double x = stack[--sp];
      double r = 0.0;
      switch (n.op) {
      case ExprOp::kNeg:
        r = -x;
        break;
      case ExprOp::kSin:
        r = std::sin(x);
        break;
      case ExprOp::kCos:
        r = std::cos(x);
        break;
      case ExprOp::kExp:
        r = std::exp(x);
        break;
      case ExprOp::kLog:
        if (!(x > 0.0)) {
          return core::Result<double>::failure("log of non-positive value");
        }
        r = std::log(x);
        break;
      case ExprOp::kSqrt:
        if (x < 0.0) {
          return core::Result<double>::failure("sqrt of negative value");
        }
        r = std::sqrt(x);
        break;
      default: // kAbs
        r = std::fabs(x);
        break;
      }
      if (!std::isfinite(r)) {
        return core::Result<double>::failure("expression evaluated to non-finite value");
      }
      stack[sp++] = r;
      break;
    }
    case ExprOp::kAdd:
    case ExprOp::kSub:
    case ExprOp::kMul:
    case ExprOp::kDiv:
    case ExprOp::kPow:
    case ExprOp::kMin:
    case ExprOp::kMax: {
      if (sp < 2) {
        return core::Result<double>::failure("invalid expression program");
      }
      const double b = stack[--sp];
      const double a = stack[--sp];
      double r = 0.0;
      switch (n.op) {
      case ExprOp::kAdd:
        r = a + b;
        break;
      case ExprOp::kSub:
        r = a - b;
        break;
      case ExprOp::kMul:
        r = a * b;
        break;
      case ExprOp::kDiv:
        if (b == 0.0) {
          return core::Result<double>::failure("division by zero in expression");
        }
        r = a / b;
        break;
      case ExprOp::kPow:
        r = std::pow(a, b);
        break;
      case ExprOp::kMin:
        r = a < b ? a : b;
        break;
      default: // kMax
        r = a > b ? a : b;
        break;
      }
      if (!std::isfinite(r)) {
        return core::Result<double>::failure("expression evaluated to non-finite value");
      }
      stack[sp++] = r;
      break;
    }
    case ExprOp::kCount:
      return core::Result<double>::failure("invalid expression program");
    }
  }
  if (sp != 1) {
    return core::Result<double>::failure("invalid expression program");
  }
  return core::Result<double>::success(stack[0]);
}

} // namespace wengine::signal

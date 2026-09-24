#pragma once

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
enum class ExprOp : std::uint8_t {
  kConst = 0, // push node.value
  kT = 1, // push t
  kAdd = 2,
  kSub = 3,
  kMul = 4,
  kDiv = 5,
  kPow = 6,
  kNeg = 7,
  kSin = 8,
  kCos = 9,
  kExp = 10,
  kLog = 11,
  kSqrt = 12,
  kAbs = 13,
  kMin = 14,
  kMax = 15,
  kCount = 16, // sentinel: valid codes are < kCount, never emitted
};

inline constexpr std::size_t kMaxExprNodes = 64;
inline constexpr std::size_t kMaxExprDepth = 32;

struct ExprNode {
  ExprOp op = ExprOp::kConst;
  double value = 0.0; // kConst payload only

  bool operator==(const ExprNode &) const noexcept = default;
};

struct ExprProgram {
  std::array<ExprNode, kMaxExprNodes> nodes{};
  std::uint8_t size = 0;

  bool operator==(const ExprProgram &) const noexcept = default;
};

namespace expr_detail {

inline constexpr double kPi = 3.141592653589793;
inline constexpr double kE = 2.718281828459045;

struct Parser {
  std::string_view in;
  std::size_t pos = 0;
  ExprProgram prog;
  std::size_t depth = 0;

  void skip_ws() noexcept {
    while (pos < in.size() &&
           (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' || in[pos] == '\r')) {
      ++pos;
    }
  }

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

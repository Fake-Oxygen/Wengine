// Lua scripting source runtime (SIG-4b): the ONE translation unit that
// knows Lua exists. Everything else (engine headers, CLI, GUI, tests)
// speaks only through `signal/lua.hpp`, which includes no Lua headers.
//
// Interpreter: vendored PUC Lua (external/lua, pinned — see release() and
// the version test), compiled as C++ like the Ubuntu `liblua*-c++`
// packages. One `lua_State` per solver instance, owned here.
//
// Sandbox (locked — see signal/lua.hpp + docs/signals.md sec. 7): only
// `math` (minus random/randomseed), `string` (minus dump/format), and a
// small pure base are visible; both libraries are read-only (C-side
// __newindex). Each evaluation runs with a FRESH `_ENV` holding the
// sandbox plus exactly four inputs (t, seed, step, dt), so scripts are
// pure functions of their inputs: no cross-step state, save/load-safe,
// deterministic. Instruction-count budget per source per step (never
// wall-clock); overrun and every other script failure map to
// kInvalidArgument — the same loud step failure as expressions and sums.
//
// All Lua API use is exception-contained: pcall catches script errors,
// and a catch-all converts anything else (e.g. allocation failure) to
// kInternal, so no exception ever crosses into engine/CLI code.

#include "wengine/signal/lua.hpp"
#include "wengine/world/state.hpp"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace wengine::lua {

namespace {

// Pinned interpreter release: asserted by the test, so a submodule bump
// fails loudly until goldens are re-verified (dependency policy).
constexpr std::string_view kPinnedRelease = "Lua 5.4.9";
static_assert(std::string_view(LUA_RELEASE) == kPinnedRelease,
              "vendored Lua release mismatch: update the pin deliberately");

constexpr int kHookGranularity = 1024; // count-hook check interval

// Base globals visible to scripts (pure/deterministic only — see header).
constexpr const char *kEnvBase[] = {"assert", "error",   "ipairs", "next",
                                    "pairs",  "pcall",   "select", "tonumber",
                                    "type",   "xpcall",  "_VERSION"};
// Base globals removed after luaopen_base (I/O, loading, GC control,
// metatables, and address-exposure channels).
constexpr const char *kDropBase[] = {
    "dofile",         "load",           "loadfile", "print",        "warn",
    "collectgarbage", "getmetatable",   "setmetatable", "tostring", "rawequal",
    "rawget",         "rawlen",         "rawset"};

int readonly_newindex(lua_State *l) {
  return luaL_error(l, "read-only library");
}

// Generic-for iterator for a proxied library: yields next, real, nil.
// `next` is order-deterministic on the pinned build for fixed contents.
int proxy_pairs(lua_State *l) {
  lua_getglobal(l, "next");
  lua_pushvalue(l, lua_upvalueindex(1)); // real table
  lua_pushnil(l);
  return 3;
}

// Replace global `libname` with an empty read-only proxy: every assignment
// (present or absent key — plain __newindex only guards absent keys, which
// is why a bare metatable is insufficient) hits the error, reads forward
// to the real table, and pairs() iterates the real contents.
void proxy_library(lua_State *l, const char *libname) {
  lua_getglobal(l, libname); // real
  lua_newtable(l); // proxy
  lua_newtable(l); // metatable
  lua_pushvalue(l, -3); // real
  lua_setfield(l, -2, "__index"); // mt.__index = real
  lua_pushcfunction(l, readonly_newindex);
  lua_setfield(l, -2, "__newindex"); // mt.__newindex = error
  lua_pushvalue(l, -3); // real
  lua_pushcclosure(l, proxy_pairs, 1);
  lua_setfield(l, -2, "__pairs"); // mt.__pairs forwards iteration
  lua_setmetatable(l, -2); // pop mt, attach to proxy
  lua_setglobal(l, libname); // global = proxy, pop proxy
  lua_pop(l, 1); // pop real
}

void strip_globals(lua_State *l) {
  for (const char *name : kDropBase) {
    lua_pushnil(l);
    lua_setglobal(l, name);
  }
  lua_getglobal(l, LUA_MATHLIBNAME); // "math"
  lua_pushnil(l);
  lua_setfield(l, -2, "random");
  lua_pushnil(l);
  lua_setfield(l, -2, "randomseed");
  lua_pop(l, 1);
  lua_getglobal(l, LUA_STRLIBNAME); // "string"
  lua_pushnil(l);
  lua_setfield(l, -2, "dump");
  lua_pushnil(l);
  lua_setfield(l, -2, "format");
  lua_pop(l, 1);
  // Proxies replace the globals (reads forward, all writes error, pairs
  // iterates the real contents). The real tables stay reachable only from
  // C (proxy __index/__pairs) — scripts cannot unwrap them: getmetatable
  // and debug are absent from the sandbox.
  proxy_library(l, LUA_MATHLIBNAME);
  proxy_library(l, LUA_STRLIBNAME);
}

void open_sandbox(lua_State *l) {
  luaL_requiref(l, "_G", luaopen_base, 1);
  lua_pop(l, 1);
  luaL_requiref(l, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(l, 1);
  luaL_requiref(l, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(l, 1);
  strip_globals(l);
}

struct HookBudget {
  std::uint64_t used = 0;
  std::uint64_t max = 65536;
};

void count_hook(lua_State *l, lua_Debug *) {
  auto *budget = *static_cast<HookBudget **>(lua_getextraspace(l));
  budget->used += static_cast<std::uint64_t>(kHookGranularity);
  if (budget->used > budget->max) {
    luaL_error(l, "instruction budget exceeded");
  }
}

// Restores the Lua stack on scope exit (every eval path balances exactly,
// failures included).
struct StackGuard {
  lua_State *l;
  int top;
  explicit StackGuard(lua_State *state) : l(state), top(lua_gettop(state)) {}
  ~StackGuard() { lua_settop(l, top); }
  StackGuard(const StackGuard &) = delete;
  StackGuard &operator=(const StackGuard &) = delete;
};

std::string stack_message(lua_State *l) {
  std::size_t len = 0;
  const char *msg = lua_tolstring(l, -1, &len);
  if (msg == nullptr || len == 0) {
    return "script failed";
  }
  return std::string(msg, len);
}

} // namespace

struct LuaSignalSolver::Impl {
  lua_State *l = nullptr;
  HookBudget hook;
  LuaConfig cfg;
  struct Entry {
    std::string text;
    int ref = LUA_REFNIL;
  };
  std::unordered_map<std::uint64_t, Entry> cache;
  std::string last_error;
  std::uint64_t evals = 0;
  std::uint64_t failures = 0;

  explicit Impl(LuaConfig c) : cfg(c) {
    static_assert(sizeof(HookBudget *) <= LUA_EXTRASPACE, "extraspace fits budget");
    l = luaL_newstate();
    if (l != nullptr) {
      *static_cast<HookBudget **>(lua_getextraspace(l)) = &hook;
      open_sandbox(l);
    }
  }

  ~Impl() {
    if (l != nullptr) {
      for (const auto &kv : cache) {
        if (kv.second.ref != LUA_REFNIL) {
          luaL_unref(l, LUA_REGISTRYINDEX, kv.second.ref);
        }
      }
      lua_close(l);
    }
  }

  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  // Compile text (fresh chunk, text mode — bytecode rejected). Returns the
  // registry ref, or LUA_REFNIL with `error` set.
  int compile(std::string_view text, std::string &error) {
    const StackGuard guard(l);
    const int rc =
        luaL_loadbufferx(l, text.data(), text.size(), "luasrc", "t");
    if (rc != LUA_OK) {
      error = stack_message(l);
      return LUA_REFNIL;
    }
    return luaL_ref(l, LUA_REGISTRYINDEX);
  }

  // Evaluate a compiled chunk with a fresh _ENV (sandbox + inputs only).
  bool eval(int ref, double t, std::uint64_t seed, std::uint64_t step, double dt,
            double &value, std::string &error) {
    const StackGuard guard(l);
    lua_rawgeti(l, LUA_REGISTRYINDEX, ref); // function
    lua_newtable(l); // fresh _ENV
    lua_pushnumber(l, t);
    lua_setfield(l, -2, "t");
    lua_pushinteger(l, static_cast<lua_Integer>(seed));
    lua_setfield(l, -2, "seed");
    lua_pushinteger(l, static_cast<lua_Integer>(step));
    lua_setfield(l, -2, "step");
    lua_pushnumber(l, dt);
    lua_setfield(l, -2, "dt");
    lua_getglobal(l, LUA_MATHLIBNAME);
    lua_setfield(l, -2, "math");
    lua_getglobal(l, LUA_STRLIBNAME);
    lua_setfield(l, -2, "string");
    for (const char *name : kEnvBase) {
      lua_getglobal(l, name);
      lua_setfield(l, -2, name);
    }
    lua_setupvalue(l, -2, 1); // pop env, attach as _ENV
    hook.used = 0;
    hook.max = (cfg.max_instructions == 0)
                   ? 0
                   : static_cast<std::uint64_t>(cfg.max_instructions);
    lua_sethook(l, count_hook, LUA_MASKCOUNT, kHookGranularity);
    const int rc = lua_pcall(l, 0, 1, 0);
    lua_sethook(l, nullptr, 0, 0);
    if (rc != LUA_OK) {
      error = stack_message(l);
      return false;
    }
    if (lua_isnumber(l, -1) == 0) {
      error = "script must return a number";
      return false;
    }
    const double v = lua_tonumber(l, -1);
    if (!std::isfinite(v)) {
      error = "script returned a non-finite value";
      return false;
    }
    value = v;
    return true;
  }

  core::Status step_state(world::State &s, double dt) {
    if (l == nullptr) {
      last_error = "Lua state unavailable";
      return {core::StatusCode::kInternal};
    }
    if (!(dt > 0.0) || !std::isfinite(dt)) {
      return {core::StatusCode::kInvalidArgument};
    }
    world::Signals &pool = s.signals;
    const double t_end = s.sim_time + dt;
    // Step number being computed (1-based, matching emitted snapshots):
    // the value written lands in the snapshot stamped step_index + 1.
    const std::uint64_t step_number = s.step_index + 1;
    for (std::uint32_t i = 0; i < pool.capacity(); ++i) {
      if (!pool.has(i) || pool.running[i] == 0) {
        continue;
      }
      if (pool.kind[i] != static_cast<std::uint8_t>(signal::SignalKind::kLua)) {
        continue;
      }
      const std::uint64_t len = pool.lua_len[i];
      const std::uint64_t off = pool.lua_off[i];
      if (len == 0 || len > signal::kMaxLuaScriptBytes ||
          off > pool.lua_text.size() || len > pool.lua_text.size() - off) {
        last_error = "corrupt Lua range";
        ++failures;
        return {core::StatusCode::kInvalidArgument};
      }
      const std::string_view text(pool.lua_text.data() + off,
                                  static_cast<std::size_t>(len));
      const std::uint64_t serial = pool.serial[i];
      if (cfg.max_instructions == 0) {
        last_error = "zero instruction budget";
        ++failures;
        return {core::StatusCode::kInvalidArgument};
      }
      auto it = cache.find(serial);
      if (it == cache.end() || std::string_view(it->second.text) != text) {
        std::string error;
        const int ref = compile(text, error);
        if (ref == LUA_REFNIL) {
          last_error = error;
          ++failures;
          return {core::StatusCode::kInvalidArgument};
        }
        if (it == cache.end()) {
          it = cache.emplace(serial, Entry{}).first;
        } else if (it->second.ref != LUA_REFNIL) {
          luaL_unref(l, LUA_REGISTRYINDEX, it->second.ref);
        }
        it->second.text.assign(text.data(), text.size());
        it->second.ref = ref;
      }
      double value = 0.0;
      std::string error;
      if (!eval(it->second.ref, t_end - pool.t0[i], serial, step_number, dt, value,
                error)) {
        last_error = error;
        ++failures;
        return {core::StatusCode::kInvalidArgument};
      }
      pool.value[i] = value;
      ++evals;
    }
    // Drop refs for despawned serials (serials never repeat, so absence in
    // the pool means gone for good). Order-independent: values unaffected.
    for (auto it = cache.begin(); it != cache.end();) {
      if (!world::find_serial(pool, it->first).has_value()) {
        if (it->second.ref != LUA_REFNIL) {
          luaL_unref(l, LUA_REGISTRYINDEX, it->second.ref);
        }
        it = cache.erase(it);
      } else {
        ++it;
      }
    }
    last_error.clear();
    return core::Status::success();
  }
};

LuaSignalSolver::LuaSignalSolver(LuaConfig cfg)
    : impl_(std::make_unique<Impl>(cfg)) {}

LuaSignalSolver::~LuaSignalSolver() = default;

core::Status LuaSignalSolver::step(world::State &s, double dt) noexcept {
  try {
    return impl_->step_state(s, dt);
  } catch (...) {
    impl_->last_error = "internal Lua failure";
    ++impl_->failures;
    return {core::StatusCode::kInternal};
  }
}

const std::string &LuaSignalSolver::last_error() const noexcept {
  return impl_->last_error;
}

std::uint64_t LuaSignalSolver::evals() const noexcept {
  return impl_->evals;
}

std::uint64_t LuaSignalSolver::failures() const noexcept {
  return impl_->failures;
}

std::string_view release() noexcept {
  return kPinnedRelease;
}

core::Status check_script(std::string_view text) {  if (text.empty() || text.size() > signal::kMaxLuaScriptBytes) {
    return {core::StatusCode::kInvalidArgument};
  }
  lua_State *l = luaL_newstate();
  if (l == nullptr) {
    return {core::StatusCode::kInternal};
  }
  core::Status st = core::Status::success();
  try {
    open_sandbox(l);
    const StackGuard guard(l);
    if (luaL_loadbufferx(l, text.data(), text.size(), "luasrc", "t") != LUA_OK) {
      st = {core::StatusCode::kInvalidArgument};
    }
  } catch (...) {
    st = {core::StatusCode::kInternal};
  }
  lua_close(l);
  return st;
}

core::Status configure_lua_source(world::Entity e, world::Signals &pool,
                                  std::string_view script,
                                  signal::QuantityId quantity, double t_now) {
  if (script.empty() || script.size() > signal::kMaxLuaScriptBytes) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (!check_script(script).ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  std::uint64_t off = 0;
  if (!world::append_lua_script(pool, script, off).ok()) {
    return {core::StatusCode::kInvalidArgument};
  }
  return world::attach_lua_source(e, pool, off,
                                  static_cast<std::uint64_t>(script.size()), quantity,
                                  t_now);
}

} // namespace wengine::lua

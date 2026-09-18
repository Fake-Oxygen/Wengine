#include "wengine/cli/complete.hpp"

namespace wengine::cli {

namespace {

void complete_names(std::string_view partial, std::span<const CommandDef> cmds,
                    std::vector<std::string> &out) {
  for (const auto &c : cmds) {
    if (c.name.substr(0, partial.size()) == partial) {
      out.emplace_back(c.name);
    }
  }
  for (std::string_view b : kBuiltinCommands) {
    if (b.substr(0, partial.size()) == partial) {
      out.emplace_back(b);
    }
  }
}

} // namespace

std::vector<std::string> complete_line(std::string_view buf, std::size_t cursor,
                                       std::span<const CommandDef> cmds) {
  if (cursor > buf.size()) {
    cursor = buf.size();
  }
  const std::string_view head = buf.substr(0, cursor);

  // Whitespace split of the text before the cursor.
  std::vector<std::string_view> toks;
  std::size_t i = 0;
  while (i < head.size()) {
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t')) {
      ++i;
    }
    if (i >= head.size()) {
      break;
    }
    std::size_t start = i;
    while (i < head.size() && head[i] != ' ' && head[i] != '\t') {
      ++i;
    }
    toks.push_back(head.substr(start, i - start));
  }
  const bool trailing_space =
      !head.empty() && (head.back() == ' ' || head.back() == '\t');

  std::vector<std::string> out;
  if (toks.empty()) {
    complete_names("", cmds, out);
    return out;
  }
  if (toks.size() == 1 && !trailing_space) {
    complete_names(toks[0], cmds, out);
    return out;
  }

  // Completing an argument of toks[0].
  std::string_view partial{};
  std::span<const std::string_view> prior{};
  if (trailing_space) {
    prior = {toks.data() + 1, toks.size() - 1};
  } else {
    partial = toks.back();
    prior = {toks.data() + 1, toks.size() - 2};
  }

  if (toks[0] == "help" && prior.empty()) {
    complete_names(partial, cmds, out);
    return out;
  }
  if (const CommandDef *def = [&] {
        for (const auto &c : cmds) {
          if (c.name == toks[0]) {
            return &c;
          }
        }
        return static_cast<const CommandDef *>(nullptr);
      }()) {
    if (def->complete != nullptr) {
      def->complete(prior, partial, out);
    }
  }
  return out;
}

std::string common_prefix(std::span<const std::string> candidates) {
  if (candidates.empty()) {
    return {};
  }
  std::string prefix = candidates.front();
  for (std::size_t k = 1; k < candidates.size(); ++k) {
    std::size_t n = 0;
    while (n < prefix.size() && n < candidates[k].size() &&
           prefix[n] == candidates[k][n]) {
      ++n;
    }
    prefix.resize(n);
    if (prefix.empty()) {
      break;
    }
  }
  return prefix;
}

} // namespace wengine::cli

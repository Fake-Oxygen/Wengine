#pragma once

#include <functional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wengine::cli {

using CommandFunction = int (*)(std::span<const std::string>, std::ostream &);

struct Command {
  std::string_view name;
  std::string_view description;
  CommandFunction function;
};

class Shell {
public:
  int execute(std::string_view line, std::ostream &out);
  void run(std::istream &in, std::ostream &out);

private:
  std::unordered_map<std::string, Command> m_commands;
  static std::vector<std::string> tokenize(std::string_view line);
  int execute_tokens(const std::vector<std::string> &tokens, std::ostream &out);
  void print_help(std::ostream &out) const;
  const Command *find_command(std::string_view name);
};

} // namespace wengine::cli

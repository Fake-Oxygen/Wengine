#include "wengine/cli/shell.hpp"
#include "wengine/cli/command_registry.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace wengine::cli {

std::vector<std::string> Shell::tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::istringstream stream{std::string{line}};
  std::string token;

  while (stream >> token) {
    tokens.push_back(std::move(token));
  }

  return tokens;
}

int Shell::execute(std::string_view line, std::ostream &out) {
  const auto tokens = tokenize(line);

  if (tokens.empty()) {
    return 0;
  }

  return execute_tokens(tokens, out);
}

const Command *Shell::find_command(std::string_view name) {
  for (const auto &command : kCommands) {
    if (command.name == name) {
      return &command;
    }
  }
  return nullptr;
}

int Shell::execute_tokens(const std::vector<std::string> &tokens, std::ostream &out) {
  const std::string &command_name = tokens[0];

  if (command_name == "exit" || command_name == "quit") {
    return -1;
  }

  if (command_name == "help") {
    print_help(out);
    return 0;
  }

  const auto *cmd = find_command(command_name);
  if (cmd == nullptr) {
    out << "Unknown command: " << command_name << '\n';
    out << "Type 'help' for available commands.\n";
    return 1;
  }

  std::span<const std::string> args{tokens.data() + 1, tokens.size() - 1};

  try {
    return cmd->function(args, out);
  } catch (const std::exception &e) {
    out << "Error: " << e.what() << '\n';
    return 1;
  }
}

void Shell::run(std::istream &in, std::ostream &out) {
  std::string line;

  while (true) {
    out << "> " << std::flush;

    if (!std::getline(in, line)) {
      break;
    }

    const int result = execute(line, out);
    if (result == -1) {
      break;
    }
  }
}

void Shell::print_help(std::ostream &out) const {
  out << "Available commands:\n\n";

  for (const auto &[name, description, func] : kCommands) {
    (void)func;
    out << "  " << name << " - " << description << '\n';
  }

  out << "\n help - Show this help\n";
  out << " exit - Exit shell\n";
}

} // namespace wengine::cli

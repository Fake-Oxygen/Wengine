#pragma once

#include <iosfwd>
#include <string_view>

namespace wengine::cli {

class Shell {
public:
  // Execute one line. `line` is copied into an internal buffer so the
  // returned views stay valid for the call. Returns 0 ok, 1 error, -1 exit.
  int execute(std::string_view line, std::ostream &out);
  void run(std::istream &in, std::ostream &out);

private:
  void print_help(std::ostream &out) const;
};

} // namespace wengine::cli

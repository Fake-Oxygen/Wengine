#include "wengine/cli/shell.hpp"
#include <iostream>

int main(int /*argc*/, char ** /*argv*/) {
  wengine::cli::Shell shell;
  shell.run(std::cin, std::cout);
  return 0;
}

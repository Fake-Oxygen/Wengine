#include "wengine/cli/commands.hpp"
#include "wengine/cli/ic_dispatch.hpp"
#include "wengine/cli/ic_registry.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

int IcSpec::run(ArgCursor &args, std::ostream &out) {
  return ic::dispatch<IcRegistry>(args, out, kName, kDescription, kUsage);
}

void IcSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                      std::vector<std::string> &out) {
  ic::complete<IcRegistry>(prior_args, partial, out);
}

} // namespace wengine::cli::commands

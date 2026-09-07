// core: ActivityRegistry — concrete mwf::IActivityRegistry.
#include "mwf_core/activity_registry.h"

namespace mwf_core {

void ActivityRegistry::registerActivity(std::string name, mwf::ActivityFn fn) {
  handlers_[std::move(name)] = std::move(fn);
}

bool ActivityRegistry::has(std::string_view name) {
  return handlers_.find(std::string(name)) != handlers_.end();
}

mwf::Result<mwf::Bytes> ActivityRegistry::invoke(std::string_view name,
                                                 const mwf::Bytes& arg_json) {
  auto it = handlers_.find(std::string(name));
  if (it == handlers_.end()) {
    return mwf::Result<mwf::Bytes>::failure("no activity registered as '" +
                                            std::string(name) + "'");
  }
  if (!it->second) {
    return mwf::Result<mwf::Bytes>::failure("activity '" + std::string(name) +
                                            "' has an empty handler");
  }
  return it->second(arg_json);
}

}  // namespace mwf_core

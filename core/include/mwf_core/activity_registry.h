// core: ActivityRegistry — concrete mwf::IActivityRegistry.
// Handler shape: (name, jsonIn) -> jsonOut.
#pragma once
#include <map>
#include <string>
#include "mwf/contracts.h"

namespace mwf_core {

// In-memory activity handler table. Application activities are registered here;
// the worker loop dispatches poll'd activity tasks through invoke().
class ActivityRegistry : public mwf::IActivityRegistry {
 public:
  void registerActivity(std::string name, mwf::ActivityFn fn) override;
  bool has(std::string_view name) override;
  mwf::Result<mwf::Bytes> invoke(std::string_view name,
                                 const mwf::Bytes& arg_json) override;

 private:
  std::map<std::string, mwf::ActivityFn> handlers_;
};

}  // namespace mwf_core

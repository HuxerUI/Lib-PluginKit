#include "csv_contract.bindings.h"

#include <pluginkit/plugin.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#if !defined(_WIN32)
#include <fstream>
#endif

namespace {

class FaultImporter final : public csv_import::TableImporter {
public:
  csv_import::ImportResult Import(const csv_import::ImportRequest& request) override {
    if (request.source == "exit") std::_Exit(5);
    if (request.source == "wait") {
#if !defined(_WIN32)
      if (const auto* marker = std::getenv("HPK_CSV_WAIT_MARKER")) std::ofstream(marker).put('\n');
#endif
      std::this_thread::sleep_for(std::chrono::minutes(1));
      std::_Exit(6);
    }
    if (request.source == "throw") throw std::runtime_error("Failure fixture");
    return {csv_import::Table{{"value"}, {{request.source}}}, {}};
  }
};

} // namespace

HPK_EXPORT_PLUGIN(csv_import::TableImporter, FaultImporter)

#ifndef SMARTGRID_HPP
#define SMARTGRID_HPP

#include "json.hpp"

#include <string>
#include <vector>

namespace sg {

Json boot();

Json run_event(int hour,
               const std::vector<std::string>& faults,
               const std::string& policy = "engine",
               const std::string& battery_policy = "dp",
               bool audit = true);

Json battery_plans();

Json evaluate(int count = 2000, unsigned seed = 2026);

int run_server(int port, const std::string& web_root);

} // namespace sg

#endif // SMARTGRID_HPP

#include "smartgrid.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <cstdint>
#include <vector>

namespace {

struct Checks {
    int passed = 0;
    int failed = 0;

    void require(bool condition, const std::string& message) {
        if (condition) {
            ++passed;
        } else {
            ++failed;
            std::cerr << "FAIL: " << message << '\n';
        }
    }
};

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

std::vector<std::string> strings(const sg::Json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value.elements()) out.push_back(item.string());
    return out;
}

std::set<std::string> string_set(const sg::Json& value) {
    const auto values = strings(value);
    return std::set<std::string>(values.begin(), values.end());
}

struct DSU {
    std::map<std::string, std::string> parent;

    explicit DSU(const sg::Json& boot) {
        for (const auto& node : boot["nodes"].elements()) {
            const std::string id = node["id"].string();
            parent[id] = id;
        }
    }
    std::string find(const std::string& x) {
        std::string root = x;
        while (parent[root] != root) root = parent[root];
        std::string cur = x;
        while (parent[cur] != cur) {
            const std::string next = parent[cur];
            parent[cur] = root;
            cur = next;
        }
        return root;
    }
    bool unite(const std::string& a, const std::string& b) {
        const std::string ra = find(a), rb = find(b);
        if (ra == rb) return false;
        parent[rb] = ra;
        return true;
    }
};

void check_event(Checks& checks, const sg::Json& boot, const sg::Json& result) {
    const auto& summary = result["summary"];
    const auto& verify = result["verify"];
    const auto tree = string_set(verify["final_lines"]);
    checks.require(summary["loop_violations"].integer() == 0, "final topology has no loops");
    checks.require(summary["multi_source_trees"].integer() == 0, "final topology has one source per tree");
    checks.require(summary["capacity_violations"].integer() == 0, "final line capacities are respected");
    checks.require(summary["total_served_mw"].number() <=
                       [&]() { double total = 0.0; for (const auto& x : summary["mesh_bound_mw"].elements()) total += x.number(); return total; }() + 1e-6,
                   "served demand stays under mesh bound");

    for (int priority = 1; priority <= 4; ++priority) {
        const std::string key = std::to_string(priority);
        const double accounted = summary["served_mw"][key].number() + summary["shed_mw"][key].number() +
                                  summary["unrestorable_mw"][key].number();
        checks.require(near(accounted, summary["demand_mw"][key].number(), 1e-6),
                       "priority demand is fully accounted for: " + key);
    }

    for (const auto& line : boot["lines"].elements()) {
        const std::string id = line["id"].string();
        const double flow = result["lines"][id]["flow_mw"].number();
        checks.require(std::abs(flow) <= line["capacity_mw"].number() + 1e-6,
                       "line flow is under rating: " + id);
    }

    std::map<std::string, std::pair<std::string, std::string>> endpoints;
    std::map<std::string, double> resistance;
    for (const auto& line : boot["lines"].elements()) {
        const std::string id = line["id"].string();
        endpoints[id] = {line["a"].string(), line["b"].string()};
        resistance[id] = line["resistance"].number();
    }
    std::map<std::string, double> balance;
    for (const auto& node : boot["nodes"].elements()) balance[node["id"].string()] = 0.0;
    for (const auto& id : tree) {
        const double flow = result["lines"][id]["flow_mw"].number();
        balance[endpoints[id].first] -= flow;
        balance[endpoints[id].second] += flow;
    }
    for (const auto& node : boot["nodes"].elements()) {
        const std::string id = node["id"].string();
        double expected = 0.0;
        if (node["kind"].string() == "load") expected = result["nodes"][id]["served_mw"].number();
        else if (result["verify"]["supply_mw"].contains(id)) expected = -result["verify"]["supply_mw"][id].number();
        checks.require(near(balance[id], expected, 1e-5), "nodal balance holds: " + id);
    }
    for (const auto& node : boot["nodes"].elements()) {
        const std::string id = node["id"].string();
        if (!result["verify"]["supply_mw"].contains(id)) continue;
        checks.require(result["verify"]["supply_mw"][id].number() <=
                           result["nodes"][id]["available_mw"].number() + 1e-3,
                       "source supply is bounded: " + id);
    }

    std::set<std::string> closed = string_set(boot["normal_lines"]);
    for (const auto& fault : summary["faults"].elements()) closed.erase(fault.string());
    for (const auto& operation : result["route"]["sequence"].elements()) {
        const std::string op = operation["op"].string();
        if (op == "open") closed.erase(operation["line"].string());
        else if (op == "close") closed.insert(operation["line"].string());
        DSU dsu(boot);
        bool radial = true;
        for (const auto& id : closed) {
            const sg::Json* selected = nullptr;
            for (const auto& candidate : boot["lines"].elements()) {
                if (candidate["id"].string() == id) { selected = &candidate; break; }
            }
            if (selected == nullptr || !dsu.unite((*selected)["a"].string(), (*selected)["b"].string())) {
                radial = false;
                break;
            }
        }
        checks.require(radial, "switching sequence remains radial");
    }
    checks.require(closed == tree, "switching sequence reaches final tree");

    for (const auto& node : boot["nodes"].elements()) {
        if (node["kind"].string() != "load") continue;
        const std::string id = node["id"].string();
        const auto& info = result["nodes"][id];
        if (info["served_mw"].number() <= 0.0) continue;
        for (const auto& line : info["path_lines"].elements())
            checks.require(tree.count(line.string()) != 0, "served load path is in final tree: " + id);
        double path_resistance = 0.0;
        for (const auto& line : info["path_lines"].elements()) path_resistance += resistance[line.string()];
        checks.require(near(path_resistance, info["path_resistance"].number(), 1e-4),
                       "shortest route resistance adds up: " + id);
    }
}

void test_boot_and_plans(Checks& checks) {
    const sg::Json boot = sg::boot();
    checks.require(boot.is_object(), "boot returns an object");
    checks.require(boot["nodes"].size() == 29, "city has 29 nodes");
    checks.require(boot["lines"].size() == 44, "city has 44 lines");
    checks.require(boot["normal_lines"].size() == 27, "normal topology has 27 lines");
    checks.require(boot["presets"].size() == 6, "boot exposes six presets");
    checks.require(boot["profiles"]["demand_mw"].size() == 24, "demand profile has 24 hours");

    const sg::Json plans = sg::battery_plans();
    for (const auto& policy : {"idle", "threshold", "dp_no_reserve", "dp"}) {
        checks.require(plans.contains(policy), std::string("battery plan exists: ") + policy);
        checks.require(plans[policy]["batteries"].size() == 3, std::string("plan has three batteries: ") + policy);
        checks.require(plans[policy]["batteries"]["BAT_N"]["soc_mwh"].size() == 25,
                       std::string("plan has 25 SOC points: ") + policy);
    }
}

void test_algorithm_traces(Checks& checks, const sg::Json& boot) {
    const sg::Json normal = sg::run_event(19, {}, "engine", "dp", true);
    checks.require(normal["summary"]["switch_ops"].integer() == 0, "normal operation needs no switching");
    checks.require(normal["route"]["algorithm"].string() == "dijkstra" ||
                       normal["route"]["algorithm"].string() == "floyd-warshall", "route chooses a shortest-path algorithm");
    checks.require(normal["detect"]["bfs_order"].size() > 0, "BFS detection returns a traversal order");
    checks.require(normal["rebuild"]["mst_lines"].size() == boot["normal_lines"].size(), "normal radial rebuild is stable");

    const sg::Json feeder = sg::run_event(10, {"L10"}, "engine", "dp", true);
    checks.require(string_set(feeder["detect"]["deenergized"]) == std::set<std::string>{"SUB_C", "HOSP", "BAT_H"},
                   "fault detection identifies the isolated central branch");
    checks.require(feeder["rebuild"]["kruskal"].size() > 0, "fault event records Kruskal candidates");
    checks.require(feeder["route"]["sequence"].size() > 0, "fault event records switching trace");
    check_event(checks, boot, feeder);
}

void test_normal_hours(Checks& checks, const sg::Json& boot) {
    for (int hour = 0; hour < 24; ++hour) {
        const sg::Json result = sg::run_event(hour, {}, "engine", "dp", true);
        checks.require(result["summary"]["switch_ops"].integer() == 0, "normal hour has no switching: " + std::to_string(hour));
        checks.require(result["summary"]["shed_mw"]["1"].number() == 0.0 &&
                           result["summary"]["shed_mw"]["2"].number() == 0.0 &&
                           result["summary"]["shed_mw"]["3"].number() == 0.0 &&
                           result["summary"]["shed_mw"]["4"].number() == 0.0,
                       "normal hour serves all demand: " + std::to_string(hour));
        check_event(checks, boot, result);
    }
}

void test_presets_and_policies(Checks& checks, const sg::Json& boot) {
    for (const auto& preset : boot["presets"].elements()) {
        std::vector<std::string> faults = strings(preset["faults"]);
        for (const auto& policy : {"proposal", "mincut", "engine", "global-exchange"}) {
            const sg::Json result = sg::run_event(static_cast<int>(preset["hour"].number()), faults, policy, "dp", true);
            check_event(checks, boot, result);
        }
    }
}

void test_random_events(Checks& checks, const sg::Json& boot) {
    std::vector<std::string> line_ids;
    for (const auto& line : boot["lines"].elements()) line_ids.push_back(line["id"].string());
    std::uint64_t state = 11;
    auto next = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    for (int i = 0; i < 150; ++i) {
        const int count = 1 + static_cast<int>(next() % 3);
        const int hour = static_cast<int>(next() % 24);
        std::set<std::string> selected;
        while (static_cast<int>(selected.size()) < count) selected.insert(line_ids[static_cast<std::size_t>(next() % line_ids.size())]);
        const std::vector<std::string> faults(selected.begin(), selected.end());
        for (const auto& policy : {"proposal", "mincut", "engine"}) {
            const sg::Json result = sg::run_event(hour, faults, policy, "dp", true);
            check_event(checks, boot, result);
        }
    }
}

void test_priority_and_island(Checks& checks, const sg::Json& boot) {
    const sg::Json short_supply = sg::run_event(19, {"L01"}, "engine", "dp", true);
    checks.require(near(short_supply["summary"]["served_mw"]["1"].number(), short_supply["summary"]["demand_mw"]["1"].number()),
                   "critical demand is served before shedding");
    checks.require(near(short_supply["summary"]["served_mw"]["2"].number(), short_supply["summary"]["demand_mw"]["2"].number()),
                   "essential demand is served before shedding");
    checks.require(short_supply["summary"]["shed_mw"]["3"].number() > 0.0,
                   "residential demand is shed when west supply is lost");
    checks.require(short_supply["summary"]["served_mw"]["4"].number() == 0.0,
                   "commercial demand is shed before residential demand");

    const sg::Json island = sg::run_event(20, {"L11", "T01"}, "engine", "dp", true);
    checks.require(strings(island["rebuild"]["islands"]) == std::vector<std::string>{"BAT_H"}, "hospital battery roots its island");
    checks.require(island["nodes"]["HOSP"]["served_mw"].number() > 0.0, "hospital island battery serves the hospital");
    for (const auto& priority : {"2", "3", "4"})
        checks.require(near(island["summary"]["served_mw"][priority].number(), island["summary"]["demand_mw"][priority].number()),
                       std::string("hospital island does not shed unrelated priority ") + priority);
    check_event(checks, boot, island);
}

} // namespace

int main() {
    try {
        Checks checks;
        const sg::Json boot = sg::boot();
        test_boot_and_plans(checks);
        test_algorithm_traces(checks, boot);
        test_normal_hours(checks, boot);
        test_presets_and_policies(checks, boot);
        test_random_events(checks, boot);
        test_priority_and_island(checks, boot);
        std::cout << checks.passed << " checks passed";
        if (checks.failed) std::cout << ", " << checks.failed << " failed";
        std::cout << '\n';
        return checks.failed == 0 ? 0 : 1;
    } catch (const std::exception& exc) {
        std::cerr << "test_cpp: " << exc.what() << '\n';
        return 2;
    }
}

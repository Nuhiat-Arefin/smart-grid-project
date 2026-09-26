#include "smartgrid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace sg {
namespace {

constexpr std::array<const char*, 4> kPriorities{{"1", "2", "3", "4"}};
constexpr std::array<const char*, 3> kPolicyNames{{"proposal", "mincut", "engine"}};
constexpr std::array<const char*, 3> kPolicyLabels{{
    "Proposal, literal pseudocode",
    "+ min-cut targeted shedding",
    "+ branch exchange (full engine)",
}};
constexpr std::array<const char*, 4> kBatteryNames{{
    "idle", "threshold", "dp_no_reserve", "dp"}};

struct Event {
    int hour = 0;
    std::vector<std::string> faults;
};

struct Preset {
    int hour = 0;
    std::vector<std::string> faults;
};

struct BatterySpec {
    std::string id;
    double energy_mwh = 0.0;
    double reserve_floor = 0.0;
    double risk_reserve = 0.0;
};

struct PolicyEvents {
    std::vector<Json> summaries;
};

const Json& member(const Json& value, const char* key) {
    static const Json null_value;
    return value.contains(key) ? value[key] : null_value;
}

const Json& element(const Json& value, std::size_t index) {
    static const Json null_value;
    return index < value.size() ? value[index] : null_value;
}

double number(const Json& value, const std::string& key, double fallback = 0.0) {
    return value.contains(key) ? value[key].number(fallback) : fallback;
}

long long integer(const Json& value, const std::string& key, long long fallback = 0) {
    return value.contains(key) ? value[key].integer(fallback) : fallback;
}

std::string string_value(const Json& value,
                         const std::string& key,
                         const std::string& fallback = {}) {
    return value.contains(key) ? value[key].string(fallback) : fallback;
}

double priority_number(const Json& value, const char* priority) {
    return value.contains(priority) ? value[priority].number(0.0) : 0.0;
}

double sum_priorities(const Json& value) {
    double total = 0.0;
    for (const char* priority : kPriorities) {
        total += priority_number(value, priority);
    }
    return total;
}

double round_digits(double value, int digits) {
    double scale = 1.0;
    for (int i = 0; i < digits; ++i) {
        scale *= 10.0;
    }
    return std::round(value * scale) / scale;
}

// Python's round(q * (n - 1)) uses ties-to-even.  The percentile positions
// used here are normally integral, but preserving the tie rule keeps small
// event-count runs aligned with the reference evaluator.
std::size_t percentile_index(std::size_t count, double quantile) {
    if (count == 0) {
        return 0;
    }
    const double position = quantile * static_cast<double>(count - 1);
    const double lower = std::floor(position);
    const double fraction = position - lower;
    double rounded = lower;
    if (fraction > 0.5 ||
        (fraction == 0.5 && std::fmod(lower, 2.0) != 0.0)) {
        rounded += 1.0;
    }
    if (rounded < 0.0) {
        return 0;
    }
    const std::size_t index = static_cast<std::size_t>(rounded);
    return std::min(count - 1, index);
}

double percentile(std::vector<double> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[percentile_index(values.size(), quantile)];
}

double mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (double value : values) {
        total += value;
    }
    return total / static_cast<double>(values.size());
}

std::vector<std::string> faults_from_json(const Json& value) {
    std::vector<std::string> faults;
    for (const Json& item : value.elements()) {
        faults.push_back(item.string());
    }
    return faults;
}

std::vector<std::string> line_ids_from_boot(const Json& snapshot) {
    std::vector<std::string> line_ids;
    const Json& lines = member(snapshot, "lines");
    for (const Json& line : lines.elements()) {
        const std::string id = string_value(line, "id");
        if (!id.empty()) {
            line_ids.push_back(id);
        }
    }
    if (line_ids.empty()) {
        const Json& normal_lines = member(snapshot, "normal_lines");
        for (const Json& item : normal_lines.elements()) {
            const std::string id = item.string();
            if (!id.empty()) {
                line_ids.push_back(id);
            }
        }
    }
    std::sort(line_ids.begin(), line_ids.end());
    line_ids.erase(std::unique(line_ids.begin(), line_ids.end()), line_ids.end());
    return line_ids;
}

std::vector<int> risk_hours_from_boot(const Json& snapshot) {
    std::vector<int> hours;
    for (const Json& item : member(snapshot, "risk_hours").elements()) {
        hours.push_back(static_cast<int>(item.integer()));
    }
    std::sort(hours.begin(), hours.end());
    hours.erase(std::unique(hours.begin(), hours.end()), hours.end());
    return hours;
}

std::vector<Preset> presets_from_boot(const Json& snapshot) {
    std::vector<Preset> presets;
    for (const Json& item : member(snapshot, "presets").elements()) {
        Preset preset;
        preset.hour = static_cast<int>(integer(item, "hour", 0));
        preset.faults = faults_from_json(member(item, "faults"));
        presets.push_back(std::move(preset));
    }
    return presets;
}

std::vector<BatterySpec> batteries_from_boot(const Json& snapshot) {
    std::vector<BatterySpec> batteries;
    for (const Json& node : member(snapshot, "nodes").elements()) {
        const Json& spec = member(node, "battery");
        if (!spec.contains("energy_mwh")) {
            continue;
        }
        BatterySpec battery;
        battery.id = string_value(node, "id");
        battery.energy_mwh = number(spec, "energy_mwh");
        battery.reserve_floor = number(spec, "reserve_floor");
        battery.risk_reserve = number(spec, "risk_reserve");
        if (!battery.id.empty() && battery.energy_mwh > 0.0) {
            batteries.push_back(std::move(battery));
        }
    }
    return batteries;
}

std::vector<Event> sample_events(const Json& snapshot, int count, unsigned seed) {
    const std::vector<std::string> line_ids = line_ids_from_boot(snapshot);
    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(count));

    // The mapping is deliberately explicit instead of relying on a library
    // distribution implementation.  It is the C++ counterpart of the
    // reference weights [0.6, 0.3, 0.1]; the output is identified in the
    // result's sampler field because it is not Python's random.Random stream.
    std::mt19937 rng(seed);
    for (int index = 0; index < count; ++index) {
        const std::uint32_t draw_for_count = rng();
        const unsigned bucket = draw_for_count % 10U;
        const std::size_t requested = bucket < 6U ? 1U : (bucket < 9U ? 2U : 3U);
        const std::size_t k = std::min(requested, line_ids.size());

        Event event;
        event.hour = static_cast<int>(rng() % 24U);
        std::vector<std::string> shuffled = line_ids;
        // Fisher-Yates with explicit modulo mapping keeps sampling behavior
        // stable across standard-library implementations.
        for (std::size_t j = shuffled.size(); j > 1; --j) {
            const std::size_t pick = static_cast<std::size_t>(rng()) % j;
            std::swap(shuffled[j - 1], shuffled[pick]);
        }
        event.faults.assign(shuffled.begin(), shuffled.begin() + k);
        std::sort(event.faults.begin(), event.faults.end());
        events.push_back(std::move(event));
    }
    return events;
}

const Json& summary_from_result(const Json& result) {
    return result.contains("summary") ? result["summary"] : result;
}

double critical_value(const Json& summary, const char* field) {
    return priority_number(member(summary, field), "1");
}

double mesh_bound_total(const Json& summary) {
    double total = 0.0;
    for (const Json& bound : member(summary, "mesh_bound_mw").elements()) {
        total += bound.number(0.0);
    }
    return total;
}

Json aggregate(const std::vector<Json>& summaries) {
    Json out = Json::object();
    const std::size_t count = summaries.size();
    out["events"] = Json(static_cast<long long>(count));

    std::array<double, 4> demand{{0.0, 0.0, 0.0, 0.0}};
    std::array<double, 4> served{{0.0, 0.0, 0.0, 0.0}};
    std::vector<double> shed;
    std::vector<double> gap;
    std::vector<double> latency;
    std::array<std::vector<double>, 4> restoration;
    shed.reserve(count);
    gap.reserve(count);
    latency.reserve(count);

    long long events_with_outage = 0;
    long long events_with_shedding = 0;
    long long loop_violations = 0;
    long long multi_source_trees = 0;
    long long capacity_violations = 0;
    long long overloads_prevented = 0;
    long long source_overloads_prevented = 0;
    long long events_with_near_limit = 0;
    long long exchanges = 0;
    long long events_with_exchange = 0;
    std::size_t critical_full = 0;

    for (const Json& summary : summaries) {
        const Json& demand_by_priority = member(summary, "demand_mw");
        const Json& served_by_priority = member(summary, "served_mw");
        const Json& shed_by_priority = member(summary, "shed_mw");
        for (std::size_t i = 0; i < kPriorities.size(); ++i) {
            demand[i] += priority_number(demand_by_priority, kPriorities[i]);
            served[i] += priority_number(served_by_priority, kPriorities[i]);
        }

        const double event_shed = sum_priorities(shed_by_priority);
        shed.push_back(event_shed);
        gap.push_back(mesh_bound_total(summary) - number(summary, "total_served_mw"));
        latency.push_back(number(summary, "latency_ms"));

        if (number(summary, "deenergized_nodes") > 0.0) {
            ++events_with_outage;
        }
        if (event_shed > 1e-9) {
            ++events_with_shedding;
        }
        if (number(summary, "critical_served_pct") >= 99.999) {
            ++critical_full;
        }
        loop_violations += integer(summary, "loop_violations");
        multi_source_trees += integer(summary, "multi_source_trees");
        capacity_violations += integer(summary, "capacity_violations");
        overloads_prevented += integer(summary, "overloads_prevented");
        source_overloads_prevented += integer(summary, "source_overloads_prevented");
        if (member(summary, "near_limit_lines").size() != 0U) {
            ++events_with_near_limit;
        }
        const long long event_exchanges = integer(summary, "exchanges");
        exchanges += event_exchanges;
        if (event_exchanges != 0) {
            ++events_with_exchange;
        }

        const Json& restoration_by_priority = member(summary, "restoration_s");
        for (std::size_t i = 0; i < kPriorities.size(); ++i) {
            const Json& restoration_item = member(restoration_by_priority, kPriorities[i]);
            if (integer(restoration_item, "count") != 0) {
                restoration[i].push_back(number(restoration_item, "max"));
            }
        }
    }

    Json served_pct = Json::object();
    for (std::size_t i = 0; i < kPriorities.size(); ++i) {
        const double pct = demand[i] != 0.0 ? 100.0 * served[i] / demand[i] : 100.0;
        served_pct[kPriorities[i]] = Json(round_digits(pct, 2));
    }
    out["served_pct"] = std::move(served_pct);
    const double critical_full_pct = count == 0
                                         ? 0.0
                                         : 100.0 * static_cast<double>(critical_full) /
                                               static_cast<double>(count);
    out["critical_full_pct"] = Json(round_digits(critical_full_pct, 2));
    out["events_with_outage"] = Json(events_with_outage);
    out["events_with_shedding"] = Json(events_with_shedding);
    out["mean_shed_mw"] = Json(round_digits(mean(shed), 3));
    out["max_shed_mw"] = Json(round_digits(shed.empty() ? 0.0
                                                        : *std::max_element(shed.begin(), shed.end()),
                                            3));
    out["mean_gap_to_bound_mw"] = Json(round_digits(mean(gap), 3));
    const std::size_t at_bound = static_cast<std::size_t>(std::count_if(
        gap.begin(), gap.end(), [](double value) { return value < 1.0; }));
    const double at_bound_pct = count == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(at_bound) /
                                          static_cast<double>(count);
    out["events_at_bound_pct"] = Json(round_digits(at_bound_pct, 2));
    out["loop_violations"] = Json(loop_violations);
    out["multi_source_trees"] = Json(multi_source_trees);
    out["capacity_violations"] = Json(capacity_violations);
    out["overloads_prevented"] = Json(overloads_prevented);
    out["source_overloads_prevented"] = Json(source_overloads_prevented);
    out["events_with_near_limit"] = Json(events_with_near_limit);
    out["exchanges"] = Json(exchanges);
    out["events_with_exchange"] = Json(events_with_exchange);
    std::vector<double> switch_ops;
    switch_ops.reserve(count);
    for (const Json& summary : summaries) {
        switch_ops.push_back(number(summary, "switch_ops"));
    }
    out["mean_switch_ops"] = Json(round_digits(mean(switch_ops), 3));

    Json restoration_out = Json::object();
    for (std::size_t i = 0; i < kPriorities.size(); ++i) {
        Json item = Json::object();
        item["events"] = Json(static_cast<long long>(restoration[i].size()));
        if (restoration[i].empty()) {
            item["median"] = Json(nullptr);
            item["p95"] = Json(nullptr);
            item["max"] = Json(nullptr);
        } else {
            item["median"] = Json(percentile(restoration[i], 0.5));
            item["p95"] = Json(percentile(restoration[i], 0.95));
            item["max"] = Json(*std::max_element(restoration[i].begin(), restoration[i].end()));
        }
        restoration_out[kPriorities[i]] = std::move(item);
    }
    out["restoration_s"] = std::move(restoration_out);

    Json latency_out = Json::object();
    latency_out["mean"] = Json(round_digits(mean(latency), 3));
    latency_out["p95"] = Json(round_digits(percentile(latency, 0.95), 3));
    latency_out["max"] = Json(round_digits(latency.empty() ? 0.0
                                                        : *std::max_element(latency.begin(), latency.end()),
                                            3));
    out["latency_ms"] = std::move(latency_out);
    return out;
}

double plan_soc(const Json& plan, const std::string& battery_id, int hour) {
    const Json& batteries = member(plan, "batteries");
    const Json& battery = member(batteries, battery_id.c_str());
    const Json& soc = member(battery, "soc_mwh");
    if (hour < 0 || static_cast<std::size_t>(hour) >= soc.size()) {
        return 0.0;
    }
    return soc[static_cast<std::size_t>(hour)].number(0.0);
}

Json unwrap_plans(Json plans) {
    if (!plans.contains("idle") && plans.contains("battery_plans")) {
        return plans["battery_plans"];
    }
    return plans;
}

Json battery_study(const Json& snapshot,
                   const Json& raw_plans,
                   const std::vector<Event>& events) {
    const Json plans = unwrap_plans(raw_plans);
    const std::vector<BatterySpec> batteries = batteries_from_boot(snapshot);
    const std::vector<int> risk_hours = risk_hours_from_boot(snapshot);
    const std::vector<Preset> presets = presets_from_boot(snapshot);
    const double idle_cost = number(member(plans, "idle"), "cost");

    struct StressCase {
        int hour = 0;
        std::vector<std::string> faults;
    };
    std::vector<StressCase> stress;
    for (const Preset& preset : presets) {
        if (preset.faults.empty()) {
            continue;
        }
        for (int hour : risk_hours) {
            stress.push_back(StressCase{hour, preset.faults});
        }
    }

    Json out = Json::object();
    for (const char* plan_name : kBatteryNames) {
        const Json& plan = member(plans, plan_name);
        std::vector<double> risk_fraction;
        long long reserve_violations = 0;
        std::vector<double> risk_energy;
        for (int hour : risk_hours) {
            double total_energy = 0.0;
            for (const BatterySpec& battery : batteries) {
                const double soc = plan_soc(plan, battery.id, hour);
                total_energy += soc;
                if (battery.energy_mwh > 0.0) {
                    risk_fraction.push_back(soc / battery.energy_mwh);
                    const double reserve_fraction = std::max(battery.reserve_floor,
                                                              battery.risk_reserve);
                    if (soc + 1e-6 < reserve_fraction * battery.energy_mwh) {
                        ++reserve_violations;
                    }
                }
            }
            risk_energy.push_back(total_energy);
        }

        std::vector<Json> all_results;
        all_results.reserve(events.size());
        for (const Event& event : events) {
            const Json result = run_event(event.hour, event.faults, "engine", plan_name, true);
            all_results.push_back(summary_from_result(result));
        }
        std::vector<Json> risk_results;
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (std::binary_search(risk_hours.begin(), risk_hours.end(), events[i].hour)) {
                risk_results.push_back(all_results[i]);
            }
        }

        double stress_served = 0.0;
        double stress_shed = 0.0;
        double stress_critical = 0.0;
        for (const StressCase& stress_case : stress) {
            const Json result = run_event(stress_case.hour, stress_case.faults,
                                          "engine", plan_name, true);
            const Json& summary = summary_from_result(result);
            stress_served += number(summary, "total_served_mw");
            stress_shed += sum_priorities(member(summary, "shed_mw"));
            stress_critical += critical_value(summary, "served_mw");
        }

        Json result = Json::object();
        result["label"] = Json(string_value(plan, "label"));
        const double cost = number(plan, "cost");
        result["cost"] = Json(round_digits(cost, 2));
        result["saving_vs_idle"] = Json(round_digits(idle_cost - cost, 2));
        const double saving_pct = idle_cost != 0.0
                                      ? 100.0 * (idle_cost - cost) / idle_cost
                                      : 0.0;
        result["saving_pct"] = Json(round_digits(saving_pct, 3));
        result["min_risk_soc_pct"] = Json(round_digits(
            100.0 * (risk_fraction.empty() ? 0.0
                                           : *std::min_element(risk_fraction.begin(),
                                                               risk_fraction.end())),
            1));
        result["reserve_violations"] = Json(reserve_violations);
        result["served_pct_all"] = aggregate(all_results)["served_pct"];
        if (risk_results.empty()) {
            result["served_pct_risk_hours"] = Json(nullptr);
        } else {
            result["served_pct_risk_hours"] = aggregate(risk_results)["served_pct"];
        }
        result["min_risk_energy_mwh"] = Json(round_digits(
            risk_energy.empty() ? 0.0
                                : *std::min_element(risk_energy.begin(), risk_energy.end()),
            2));
        result["stress_cases"] = Json(static_cast<long long>(stress.size()));
        result["stress_served_mw"] = Json(round_digits(stress_served, 2));
        result["stress_shed_mw"] = Json(round_digits(stress_shed, 2));
        result["stress_critical_mw"] = Json(round_digits(stress_critical, 2));
        out[plan_name] = std::move(result);
    }
    return out;
}

std::string compiler_metadata() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return std::string("msvc ") + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

} // namespace

Json evaluate(int count, unsigned seed) {
    if (count < 1) {
        count = 1;
    }

    const Json snapshot = boot();
    const Json plans = battery_plans();
    const std::vector<Event> events = sample_events(snapshot, count, seed);

    std::array<PolicyEvents, kPolicyNames.size()> policy_events;
    for (std::size_t policy_index = 0; policy_index < kPolicyNames.size(); ++policy_index) {
        PolicyEvents& results = policy_events[policy_index];
        results.summaries.reserve(events.size());
        for (const Event& event : events) {
            const Json result = run_event(event.hour, event.faults,
                                          kPolicyNames[policy_index], "dp", true);
            results.summaries.push_back(summary_from_result(result));
        }
    }

    std::array<Json, kPolicyNames.size()> policy_aggregates;
    for (std::size_t policy_index = 0; policy_index < kPolicyNames.size(); ++policy_index) {
        policy_aggregates[policy_index] = aggregate(policy_events[policy_index].summaries);
        policy_aggregates[policy_index]["label"] = Json(kPolicyLabels[policy_index]);
    }

    Json paired_events = Json::array();
    std::vector<double> gains;
    gains.reserve(events.size());
    std::vector<double> critical_shortfalls;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Json& proposal = policy_events[0].summaries[i];
        const Json& mincut = policy_events[1].summaries[i];
        const Json& engine = policy_events[2].summaries[i];
        Json pair = Json::object();
        pair["hour"] = Json(static_cast<long long>(events[i].hour));
        Json faults = Json::array();
        for (const std::string& fault : events[i].faults) {
            faults.push_back(Json(fault));
        }
        pair["faults"] = std::move(faults);
        const std::array<const Json*, 3> summaries{{&proposal, &mincut, &engine}};
        for (std::size_t policy_index = 0; policy_index < summaries.size(); ++policy_index) {
            const std::string key = std::string("served_") + kPolicyNames[policy_index];
            pair[key] = Json(round_digits(number(*summaries[policy_index], "total_served_mw"), 3));
        }
        pair["bound"] = Json(round_digits(mesh_bound_total(engine), 3));
        pair["critical_demand"] = Json(critical_value(engine, "demand_mw"));
        const Json& engine_bound = member(engine, "mesh_bound_mw");
        pair["critical_bound"] = Json(element(engine_bound, 0).number(0.0));
        for (std::size_t policy_index = 0; policy_index < summaries.size(); ++policy_index) {
            const std::string key = std::string("critical_") + kPolicyNames[policy_index];
            pair[key] = Json(critical_value(*summaries[policy_index], "served_mw"));
        }
        pair["demand"] = Json(number(engine, "total_demand_mw"));
        gains.push_back(number(engine, "total_served_mw") - number(proposal, "total_served_mw"));
        if (critical_value(engine, "served_mw") < critical_value(engine, "demand_mw") - 1e-6) {
            critical_shortfalls.push_back(
                element(engine_bound, 0).number(0.0) - critical_value(engine, "served_mw"));
        }
        paired_events.push_back(std::move(pair));
    }

    const std::size_t engine_better = static_cast<std::size_t>(std::count_if(
        gains.begin(), gains.end(), [](double value) { return value > 1e-6; }));
    const std::size_t proposal_better = static_cast<std::size_t>(std::count_if(
        gains.begin(), gains.end(), [](double value) { return value < -1e-6; }));
    const double max_engine_gain = gains.empty()
                                      ? 0.0
                                      : *std::max_element(gains.begin(), gains.end());
    const double max_proposal_gain = gains.empty()
                                         ? 0.0
                                         : -*std::min_element(gains.begin(), gains.end());
    std::vector<double> positive_gains;
    for (double gain : gains) {
        if (gain > 1e-6) {
            positive_gains.push_back(gain);
        }
    }

    Json paired = Json::object();
    paired["engine_better"] = Json(static_cast<long long>(engine_better));
    paired["proposal_better"] = Json(static_cast<long long>(proposal_better));
    paired["max_engine_gain_mw"] = Json(round_digits(max_engine_gain, 3));
    paired["mean_engine_gain_mw"] = Json(round_digits(mean(positive_gains), 3));
    paired["max_proposal_gain_mw"] = Json(round_digits(max_proposal_gain, 3));
    paired["critical_shortfall_events"] = Json(static_cast<long long>(critical_shortfalls.size()));
    paired["critical_shortfall_beyond_bound_mw"] = Json(round_digits(
        critical_shortfalls.empty()
            ? 0.0
            : *std::max_element(critical_shortfalls.begin(), critical_shortfalls.end()),
        3));

    Json policies = Json::object();
    for (std::size_t policy_index = 0; policy_index < kPolicyNames.size(); ++policy_index) {
        policies[kPolicyNames[policy_index]] = std::move(policy_aggregates[policy_index]);
    }

    Json result = Json::object();
    result["events"] = Json(static_cast<long long>(events.size()));
    result["seed"] = Json(static_cast<long long>(seed));
    result["implementation"] = Json("C++17");
    result["sampler"] = Json("std::mt19937 seed; modulo-10 weights 6/3/1; modulo-24 hour; explicit Fisher-Yates line sample (v1)");
    Json runtime = Json::object();
    runtime["language"] = Json("C++17");
    runtime["compiler"] = Json(compiler_metadata());
    runtime["standard"] = Json(static_cast<long long>(__cplusplus));
#if defined(__APPLE__)
    runtime["platform"] = Json("macOS");
#elif defined(__linux__)
    runtime["platform"] = Json("Linux");
#elif defined(_WIN32)
    runtime["platform"] = Json("Windows");
#else
    runtime["platform"] = Json("unknown");
#endif
    result["runtime"] = std::move(runtime);
    result["policies"] = std::move(policies);
    result["paired"] = std::move(paired);
    result["battery"] = battery_study(snapshot, plans, events);
    result["per_event"] = std::move(paired_events);
    return result;
}

} // namespace sg

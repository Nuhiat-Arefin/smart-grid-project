#include "smartgrid.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <stdexcept>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sg {
namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr int HOURS = 24;
constexpr const char* ROOT = "__ROOT__";
constexpr const char* SOURCE = "__S__";
constexpr const char* SINK = "__T__";

double rround(double x, int places) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream << std::setprecision(places) << x;
    return std::stod(stream.str());
}

double r3(double x) { return rround(x, 3); }
double r4(double x) { return rround(x, 4); }
double r2(double x) { return rround(x, 2); }
long long iround(double x) { return static_cast<long long>(std::nearbyint(x)); }
long long kw(double mw) { return iround(mw * 1000.0); }

Json jstr(const std::string& value) { return Json(value); }
Json jnum(double value) { return Json(value); }
Json jint(long long value) { return Json(value); }
Json jbool(bool value) { return Json(value); }
Json jnull() { return Json(nullptr); }

Json jstrings(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const auto& value : values) out.push_back(jstr(value));
    return out;
}

Json jnumbers(const std::vector<double>& values, int places = -1) {
    Json out = Json::array();
    for (double value : values) out.push_back(jnum(places < 0 ? value : rround(value, places)));
    return out;
}

Json jint_array(const std::vector<long long>& values) {
    Json out = Json::array();
    for (long long value : values) out.push_back(jint(value));
    return out;
}

enum class Kind { Plant, Solar, Wind, Battery, Substation, Load };

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Plant: return "plant";
    case Kind::Solar: return "solar";
    case Kind::Wind: return "wind";
    case Kind::Battery: return "battery";
    case Kind::Substation: return "substation";
    case Kind::Load: return "load";
    }
    return "";
}

struct BatterySpec {
    double energy_mwh = 0.0;
    double charge_mw = 0.0;
    double discharge_mw = 0.0;
    double efficiency_charge = 0.95;
    double efficiency_discharge = 0.95;
    double initial_soc = 0.5;
    double reserve_floor = 0.2;
    double risk_reserve = 0.5;
};

struct Node {
    std::string id;
    std::string name;
    Kind kind;
    double x = 0.0;
    double y = 0.0;
    double capacity_mw = 0.0;
    double peak_mw = 0.0;
    int priority = 0;
    std::string category;
    int blocks = 0;
    std::optional<BatterySpec> battery;
};

struct Line {
    std::string id;
    std::string a;
    std::string b;
    double resistance = 0.0;
    double capacity_mw = 0.0;
    bool tie = false;
};

struct Grid {
    std::string name;
    std::vector<Node> nodes;
    std::map<std::string, std::size_t> node_index;
    std::vector<Line> lines;
    std::map<std::string, std::size_t> line_index;
    std::vector<double> solar_factor;
    std::vector<double> wind_factor;
    std::vector<std::pair<std::string, std::vector<double>>> demand_factor;
    std::map<std::string, std::size_t> demand_index;
    std::vector<double> price;
    std::vector<double> export_price;
    std::set<int> risk_hours;
    double nominal_kv = 33.0;

    const Node& node(const std::string& id) const { return nodes.at(node_index.at(id)); }
    const Line& line(const std::string& id) const { return lines.at(line_index.at(id)); }
    Node& node(const std::string& id) { return nodes.at(node_index.at(id)); }
    std::vector<std::string> ids(Kind kind) const {
        std::vector<std::string> out;
        for (const auto& n : nodes) if (n.kind == kind) out.push_back(n.id);
        return out;
    }
    std::vector<std::string> loads() const { return ids(Kind::Load); }
    std::vector<std::string> plants() const { return ids(Kind::Plant); }
    std::vector<std::string> renewables() const {
        auto out = ids(Kind::Solar);
        auto wind = ids(Kind::Wind);
        out.insert(out.end(), wind.begin(), wind.end());
        return out;
    }
    std::vector<std::string> batteries() const { return ids(Kind::Battery); }
    double demand_mw(const std::string& id, int hour) const {
        const Node& n = node(id);
        if (n.kind != Kind::Load) return 0.0;
        return n.peak_mw * demand_factor.at(demand_index.at(n.category)).second.at(static_cast<std::size_t>(hour));
    }
    double generation_mw(const std::string& id, int hour) const {
        const Node& n = node(id);
        if (n.kind == Kind::Plant) return n.capacity_mw;
        if (n.kind == Kind::Solar) return n.capacity_mw * solar_factor.at(static_cast<std::size_t>(hour));
        if (n.kind == Kind::Wind) return n.capacity_mw * wind_factor.at(static_cast<std::size_t>(hour));
        return 0.0;
    }
    double total_demand_mw(int hour) const {
        double out = 0.0;
        for (const auto& id : loads()) out += demand_mw(id, hour);
        return out;
    }
    double renewable_mw(int hour) const {
        double out = 0.0;
        for (const auto& id : renewables()) out += generation_mw(id, hour);
        return out;
    }
    std::map<std::string, std::vector<std::pair<std::string, std::string>>>
    adjacency_map(const std::set<std::string>& line_ids) const {
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> adj;
        for (const auto& n : nodes) adj.emplace(n.id, std::vector<std::pair<std::string, std::string>>{});
        std::vector<std::string> sorted(line_ids.begin(), line_ids.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& id : sorted) {
            const auto& e = line(id);
            adj[e.a].push_back({e.b, id});
            adj[e.b].push_back({e.a, id});
        }
        return adj;
    }
    std::vector<double> reserve_mwh(const std::string& id, bool with_risk = true) const {
        const auto& spec = *node(id).battery;
        std::vector<double> out;
        for (int t = 0; t <= HOURS; ++t) {
            double frac = spec.reserve_floor;
            if (with_risk && risk_hours.count(t)) frac = std::max(frac, spec.risk_reserve);
            out.push_back(frac * spec.energy_mwh);
        }
        return out;
    }
};

Grid make_city() {
    Grid g;
    g.name = "Demo City 33 kV distribution network";
    auto add_node = [&](const std::string& id, const std::string& name, Kind kind, double x, double y,
                        double capacity = 0.0, double peak = 0.0, int priority = 0,
                        const std::string& category = "", int blocks = 0,
                        std::optional<BatterySpec> battery = std::nullopt) {
        g.node_index[id] = g.nodes.size();
        g.nodes.push_back({id, name, kind, x, y, capacity, peak, priority, category, blocks, battery});
    };
    add_node("GAS", "West Gas Plant", Kind::Plant, 60, 310, 85);
    add_node("CCGT", "East Power Station", Kind::Plant, 945, 310, 70);
    add_node("SOLAR", "Solar Farm", Kind::Solar, 560, 40, 45);
    add_node("WIND", "Wind Farm", Kind::Wind, 470, 590, 35);
    add_node("SUB_W", "West Substation", Kind::Substation, 170, 310);
    add_node("SUB_N", "North Substation", Kind::Substation, 360, 150);
    add_node("SUB_C", "Central Substation", Kind::Substation, 500, 300);
    add_node("SUB_S", "South Substation", Kind::Substation, 340, 470);
    add_node("SUB_NE", "North-East Substation", Kind::Substation, 700, 150);
    add_node("SUB_E", "East Substation", Kind::Substation, 830, 310);
    add_node("SUB_SE", "South-East Substation", Kind::Substation, 680, 470);
    add_node("BAT_N", "North Battery", Kind::Battery, 240, 70, 0, 0, 0, "", 0,
             BatterySpec{20, 5, 5, 0.95, 0.95, 0.5, 0.2, 0.5});
    add_node("BAT_S", "South Battery", Kind::Battery, 220, 560, 0, 0, 0, "", 0,
             BatterySpec{20, 5, 5, 0.95, 0.95, 0.5, 0.2, 0.5});
    add_node("BAT_H", "Hospital Battery", Kind::Battery, 575, 225, 0, 0, 0, "", 0,
             BatterySpec{16, 4, 4, 0.95, 0.95, 0.75, 0.6, 0.6});
    add_node("HOSP", "City General Hospital", Kind::Load, 500, 185, 0, 12, 1, "hospital", 6);
    add_node("WATER", "Water Treatment Works", Kind::Load, 230, 440, 0, 8, 1, "water", 4);
    add_node("EMERG", "Emergency Services HQ", Kind::Load, 790, 420, 0, 5, 1, "emergency", 2);
    add_node("SCHOOL", "Central School", Kind::Load, 300, 250, 0, 5, 2, "school", 2);
    add_node("TELECOM", "Telecom Exchange", Kind::Load, 410, 235, 0, 4, 2, "telecom", 2);
    add_node("UNI", "University Campus", Kind::Load, 720, 250, 0, 10, 2, "university", 5);
    add_node("METRO", "Metro Rail Depot", Kind::Load, 880, 170, 0, 8, 2, "metro", 4);
    add_node("HILL", "Hillside", Kind::Load, 130, 170, 0, 12, 3, "residential", 6);
    add_node("OLDTOWN", "Old Town", Kind::Load, 300, 340, 0, 18, 3, "residential", 9);
    add_node("RIVER", "Riverside Homes", Kind::Load, 520, 520, 0, 16, 3, "residential", 8);
    add_node("GREEN", "Green Park", Kind::Load, 850, 500, 0, 14, 3, "residential", 7);
    add_node("LAKE", "Lakeview", Kind::Load, 780, 60, 0, 15, 3, "residential", 8);
    add_node("MALL", "Central Mall", Kind::Load, 420, 390, 0, 14, 4, "commercial", 7);
    add_node("BIZ", "Business District", Kind::Load, 640, 330, 0, 20, 4, "commercial", 10);
    add_node("INDUS", "Industrial Park", Kind::Load, 110, 460, 0, 22, 4, "industrial", 11);

    const double solar[] = {0, 0, 0, 0, 0, 0, 0.05, 0.18, 0.38, 0.58, 0.75, 0.88,
                            0.95, 0.93, 0.84, 0.68, 0.47, 0.24, 0.06, 0, 0, 0, 0, 0};
    const double wind[] = {0.72, 0.75, 0.78, 0.76, 0.70, 0.64, 0.55, 0.46, 0.40, 0.36, 0.33, 0.31,
                           0.30, 0.32, 0.36, 0.42, 0.50, 0.56, 0.60, 0.62, 0.65, 0.68, 0.70, 0.71};
    const double price[] = {42, 40, 38, 38, 40, 48, 65, 85, 92, 80, 70, 62,
                            55, 55, 60, 72, 95, 125, 150, 160, 148, 120, 85, 55};
    g.solar_factor.assign(std::begin(solar), std::end(solar));
    g.wind_factor.assign(std::begin(wind), std::end(wind));
    g.price.assign(std::begin(price), std::end(price));
    g.export_price.assign(24, 15.0);
    for (int h = 17; h < 23; ++h) g.risk_hours.insert(h);

    g.demand_factor = {
        {"residential", {0.50, 0.46, 0.44, 0.43, 0.44, 0.50, 0.62, 0.74, 0.72, 0.62, 0.56, 0.55,
                          0.56, 0.55, 0.55, 0.58, 0.66, 0.80, 0.93, 1.00, 0.98, 0.90, 0.75, 0.60}},
        {"commercial", {0.30, 0.28, 0.27, 0.27, 0.28, 0.30, 0.38, 0.52, 0.72, 0.88, 0.96, 1.00,
                         1.00, 0.98, 0.97, 0.95, 0.92, 0.88, 0.82, 0.74, 0.62, 0.48, 0.38, 0.33}},
        {"industrial", {0.62, 0.60, 0.60, 0.60, 0.62, 0.68, 0.80, 0.92, 0.98, 1.00, 1.00, 0.98,
                         0.95, 0.98, 1.00, 1.00, 0.96, 0.88, 0.78, 0.72, 0.68, 0.66, 0.64, 0.63}},
        {"hospital", {0.82, 0.80, 0.80, 0.80, 0.80, 0.82, 0.86, 0.92, 0.97, 1.00, 1.00, 1.00,
                       0.99, 0.99, 1.00, 0.99, 0.97, 0.95, 0.94, 0.93, 0.91, 0.88, 0.85, 0.83}},
        {"water", {0.70, 0.68, 0.66, 0.66, 0.70, 0.80, 0.92, 1.00, 0.98, 0.92, 0.88, 0.86,
                    0.86, 0.86, 0.86, 0.88, 0.92, 0.98, 1.00, 0.98, 0.92, 0.85, 0.78, 0.72}},
        {"emergency", {0.85, 0.84, 0.84, 0.84, 0.85, 0.86, 0.88, 0.92, 0.95, 0.97, 0.98, 0.98,
                        0.98, 0.98, 0.98, 0.98, 0.97, 0.96, 0.95, 0.94, 0.92, 0.90, 0.88, 0.86}},
        {"school", {0.15, 0.15, 0.15, 0.15, 0.15, 0.18, 0.30, 0.65, 0.95, 1.00, 1.00, 1.00,
                     0.98, 1.00, 0.92, 0.70, 0.40, 0.28, 0.22, 0.20, 0.18, 0.17, 0.16, 0.15}},
        {"university", {0.35, 0.33, 0.32, 0.32, 0.33, 0.36, 0.45, 0.65, 0.85, 0.95, 1.00, 1.00,
                         0.98, 1.00, 1.00, 0.96, 0.88, 0.78, 0.70, 0.64, 0.58, 0.50, 0.42, 0.38}},
        {"telecom", {0.90, 0.89, 0.88, 0.88, 0.88, 0.89, 0.91, 0.94, 0.96, 0.97, 0.98, 0.99,
                      0.99, 0.99, 0.99, 0.99, 0.98, 0.98, 0.99, 1.00, 1.00, 0.98, 0.95, 0.92}},
        {"metro", {0.20, 0.18, 0.18, 0.18, 0.22, 0.40, 0.75, 0.98, 1.00, 0.82, 0.62, 0.58,
                    0.60, 0.58, 0.60, 0.68, 0.85, 0.98, 1.00, 0.86, 0.66, 0.50, 0.38, 0.26}},
    };
    for (std::size_t i = 0; i < g.demand_factor.size(); ++i) g.demand_index[g.demand_factor[i].first] = i;

    struct LDef { const char* id; const char* a; const char* b; double capacity; bool tie; };
    const LDef defs[] = {
        {"L01", "GAS", "SUB_W", 100, false}, {"L02", "SUB_W", "HILL", 25, false},
        {"L03", "SUB_W", "OLDTOWN", 30, false}, {"L04", "SUB_W", "INDUS", 35, false},
        {"L05", "SUB_W", "SUB_N", 60, false}, {"L06", "SUB_W", "SUB_S", 60, false},
        {"L07", "SUB_N", "SCHOOL", 15, false}, {"L08", "SUB_N", "BAT_N", 12, false},
        {"L09", "SUB_N", "TELECOM", 15, false}, {"L10", "SUB_N", "SUB_C", 45, false},
        {"L11", "SUB_C", "HOSP", 25, false}, {"L12", "HOSP", "BAT_H", 10, false},
        {"L13", "SUB_S", "WATER", 20, false}, {"L14", "SUB_S", "BAT_S", 12, false},
        {"L15", "SUB_S", "RIVER", 30, false}, {"L16", "SUB_S", "WIND", 40, false},
        {"L17", "SUB_S", "MALL", 25, false}, {"L18", "CCGT", "SUB_E", 90, false},
        {"L19", "SUB_E", "SUB_NE", 60, false}, {"L20", "SUB_NE", "SOLAR", 50, false},
        {"L21", "SUB_NE", "LAKE", 25, false}, {"L22", "SUB_NE", "UNI", 20, false},
        {"L23", "SUB_E", "METRO", 15, false}, {"L24", "SUB_E", "SUB_SE", 50, false},
        {"L25", "SUB_SE", "EMERG", 12, false}, {"L26", "SUB_SE", "GREEN", 25, false},
        {"L27", "SUB_E", "BIZ", 30, false}, {"T01", "HOSP", "SUB_NE", 25, true},
        {"T02", "SUB_C", "BIZ", 30, true}, {"T03", "RIVER", "SUB_SE", 25, true},
        {"T04", "SCHOOL", "HILL", 15, true}, {"T05", "BAT_N", "HILL", 12, true},
        {"T06", "OLDTOWN", "MALL", 25, true}, {"T07", "WATER", "INDUS", 20, true},
        {"T08", "BAT_S", "INDUS", 12, true}, {"T09", "OLDTOWN", "WATER", 20, true},
        {"T10", "TELECOM", "SUB_C", 15, true}, {"T11", "UNI", "SUB_E", 20, true},
        {"T12", "LAKE", "METRO", 20, true}, {"T13", "EMERG", "SUB_E", 12, true},
        {"T14", "GREEN", "SUB_E", 25, true}, {"T15", "WIND", "SUB_SE", 40, true},
        {"T16", "SOLAR", "SUB_N", 50, true}, {"T17", "SUB_C", "MALL", 20, true},
    };
    for (const auto& d : defs) {
        const auto& a = g.node(d.a);
        const auto& b = g.node(d.b);
        const double grade = d.tie ? 2.2 : 1.0;
        const double resistance = r4(0.0016 * grade * std::hypot(a.x - b.x, a.y - b.y));
        g.line_index[d.id] = g.lines.size();
        g.lines.push_back({d.id, d.a, d.b, resistance, d.capacity, d.tie});
    }
    return g;
}

const Grid& city() {
    static const Grid g = make_city();
    return g;
}

struct BFSResult {
    std::vector<std::string> order;
    std::map<std::string, std::string> parent;
    std::map<std::string, std::string> parent_line;
    std::map<std::string, int> hops;
    std::map<std::string, std::string> origin;

    bool reached(const std::string& id) const { return parent.count(id) != 0; }
    std::vector<std::string> path_to(const std::string& id) const {
        std::vector<std::string> path;
        auto it = parent.find(id);
        if (it == parent.end()) return path;
        std::string cur = id;
        while (true) {
            path.push_back(cur);
            auto p = parent.find(cur);
            if (p == parent.end() || p->second.empty()) break;
            cur = p->second;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }
};

BFSResult bfs(const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& adj,
             const std::vector<std::string>& sources) {
    BFSResult result;
    std::deque<std::string> queue;
    for (const auto& source : sources) {
        if (adj.count(source) && !result.parent.count(source)) {
            result.parent[source] = "";
            result.parent_line[source] = "";
            result.hops[source] = 0;
            result.origin[source] = source;
            queue.push_back(source);
        }
    }
    while (!queue.empty()) {
        std::string u = queue.front();
        queue.pop_front();
        result.order.push_back(u);
        for (const auto& edge : adj.at(u)) {
            const std::string& v = edge.first;
            if (!result.parent.count(v)) {
                result.parent[v] = u;
                result.parent_line[v] = edge.second;
                result.hops[v] = result.hops[u] + 1;
                result.origin[v] = result.origin[u];
                queue.push_back(v);
            }
        }
    }
    return result;
}

class DisjointSet {
public:
    explicit DisjointSet(const std::vector<std::string>& items = {}) {
        for (const auto& item : items) add(item);
    }
    void add(const std::string& item) {
        if (!parent_.count(item)) {
            parent_[item] = item;
            rank_[item] = 0;
        }
    }
    std::string find(const std::string& item) {
        std::string root = item;
        while (parent_.at(root) != root) root = parent_.at(root);
        std::string x = item;
        while (parent_.at(x) != root) {
            std::string next = parent_.at(x);
            parent_[x] = root;
            x = next;
        }
        return root;
    }
    bool unite(const std::string& a, const std::string& b) {
        std::string ra = find(a), rb = find(b);
        if (ra == rb) return false;
        if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
        parent_[rb] = ra;
        if (rank_[ra] == rank_[rb]) ++rank_[ra];
        return true;
    }
    bool connected(const std::string& a, const std::string& b) { return find(a) == find(b); }

private:
    std::map<std::string, std::string> parent_;
    std::map<std::string, int> rank_;
};

struct KruskalStep {
    std::string kind;
    std::string item;
    std::string a;
    std::string b;
    std::optional<double> weight;
    bool accepted = false;
};

struct RadialForest {
    std::vector<std::string> lines;
    std::vector<std::string> roots;
    std::vector<std::string> island_roots;
    double total_weight = 0.0;
    std::vector<KruskalStep> steps;
};

RadialForest kruskal_radial(const std::vector<std::string>& nodes,
                            const std::vector<Line>& lines,
                            const std::vector<std::string>& primary,
                            const std::vector<std::string>& backups = {}) {
    struct Candidate {
        int rank;
        double weight;
        std::string id;
        std::string kind;
        std::string a;
        std::string b;
        std::optional<double> step_weight;
        const Line* line = nullptr;
    };
    std::vector<Candidate> candidates;
    for (const auto& s : primary) candidates.push_back({0, 0.0, s, "source", ROOT, s, 0.0, nullptr});
    for (const auto& e : lines) candidates.push_back({1, e.resistance, e.id, "line", e.a, e.b, e.resistance, &e});
    for (const auto& b : backups) candidates.push_back({2, 0.0, b, "backup", ROOT, b, std::nullopt, nullptr});
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
        return std::tie(x.rank, x.weight, x.id) < std::tie(y.rank, y.weight, y.id);
    });
    std::vector<std::string> dsu_nodes = nodes;
    dsu_nodes.push_back(ROOT);
    DisjointSet dsu(dsu_nodes);
    RadialForest forest;
    for (const auto& c : candidates) {
        const bool accepted = dsu.unite(c.a, c.b);
        forest.steps.push_back({c.kind, c.id, c.a, c.b, c.step_weight, accepted});
        if (!accepted) continue;
        if (c.kind == "line") {
            forest.lines.push_back(c.id);
            forest.total_weight += c.weight;
        } else {
            forest.roots.push_back(c.id);
            if (c.kind == "backup") forest.island_roots.push_back(c.id);
        }
    }
    return forest;
}

class FlowNetwork {
public:
    struct ArcArrays {
        std::vector<int> head;
        std::vector<long long> cap;
        std::vector<long long> flow;
    };

    int node(const std::string& label) {
        auto it = index.find(label);
        if (it != index.end()) return it->second;
        const int id = static_cast<int>(labels.size());
        index[label] = id;
        labels.push_back(label);
        adj.emplace_back();
        return id;
    }

    int add_edge(const std::string& u, const std::string& v, long long capacity,
                 long long reverse_capacity = 0) {
        const int iu = node(u), iv = node(v);
        const int e = static_cast<int>(head.size());
        head.push_back(iv); head.push_back(iu);
        cap.push_back(capacity); cap.push_back(reverse_capacity);
        flow.push_back(0); flow.push_back(0);
        adj[iu].push_back(e); adj[iv].push_back(e + 1);
        return e;
    }

    void set_capacity(int e, long long capacity) { cap.at(static_cast<std::size_t>(e)) = capacity; }
    const std::string& tail(int e) const { return labels.at(static_cast<std::size_t>(head.at(static_cast<std::size_t>(e ^ 1)))); }

    long long max_flow(const std::string& source, const std::string& sink) {
        if (!index.count(source) || !index.count(sink)) return 0;
        const int s = index.at(source), t = index.at(sink);
        long long added = 0;
        while (true) {
            std::vector<int> via(labels.size(), -1);
            via[s] = -2;
            std::deque<int> q;
            q.push_back(s);
            while (!q.empty() && via[t] == -1) {
                const int u = q.front(); q.pop_front();
                for (const int e : adj[u]) {
                    const int v = head[e];
                    if (via[v] == -1 && cap[e] - flow[e] > 0) {
                        via[v] = e;
                        q.push_back(v);
                    }
                }
            }
            if (via[t] == -1) return added;
            long long bottleneck = std::numeric_limits<long long>::max();
            int v = t;
            while (v != s) {
                const int e = via[v];
                bottleneck = std::min(bottleneck, cap[e] - flow[e]);
                v = head[e ^ 1];
            }
            v = t;
            while (v != s) {
                const int e = via[v];
                flow[e] += bottleneck;
                flow[e ^ 1] -= bottleneck;
                v = head[e ^ 1];
            }
            added += bottleneck;
        }
    }

    long long value(const std::string& source) const {
        if (!index.count(source)) return 0;
        long long out = 0;
        for (const int e : adj.at(static_cast<std::size_t>(index.at(source)))) out += flow[e];
        return out;
    }

    std::set<std::string> min_cut_source_side(const std::string& source) const {
        std::set<std::string> out;
        if (!index.count(source)) return out;
        const int s = index.at(source);
        std::set<int> seen;
        std::deque<int> q;
        seen.insert(s); q.push_back(s);
        while (!q.empty()) {
            const int u = q.front(); q.pop_front();
            for (const int e : adj[u]) {
                const int v = head[e];
                if (!seen.count(v) && cap[e] - flow[e] > 0) {
                    seen.insert(v); q.push_back(v);
                }
            }
        }
        for (int id : seen) out.insert(labels[id]);
        return out;
    }

    std::vector<int> cut_arcs(const std::set<std::string>& side) const {
        std::vector<int> out;
        for (int e = 0; e < static_cast<int>(head.size()); ++e) {
            if (cap[e] > 0 && side.count(tail(e)) && !side.count(labels[head[e]])) out.push_back(e);
        }
        return out;
    }

    std::map<std::string, int> index;
    std::vector<std::string> labels;
    std::vector<std::vector<int>> adj;
    std::vector<int> head;
    std::vector<long long> cap;
    std::vector<long long> flow;
};

struct ShortestPaths {
    std::map<std::string, double> dist;
    std::map<std::string, std::string> parent;
    std::map<std::string, std::string> parent_line;
    std::map<std::string, std::string> root;
    std::string algorithm;

    std::vector<std::string> path_to(const std::string& id) const {
        if (!dist.count(id)) return {};
        std::vector<std::string> path;
        std::string cur = id;
        while (true) {
            path.push_back(cur);
            auto it = parent.find(cur);
            if (it == parent.end() || it->second.empty()) break;
            cur = it->second;
        }
        std::reverse(path.begin(), path.end());
        return path;
    }
    std::vector<std::string> lines_to(const std::string& id) const {
        if (!dist.count(id)) return {};
        std::vector<std::string> out;
        std::string cur = id;
        while (parent.count(cur) && !parent.at(cur).empty()) {
            out.push_back(parent_line.at(cur));
            cur = parent.at(cur);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
};

ShortestPaths dijkstra(const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& adj,
                       const std::vector<std::string>& sources,
                       const std::function<double(const std::string&)>& weight) {
    ShortestPaths out;
    out.algorithm = "dijkstra";
    using Item = std::pair<double, std::string>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (const auto& s : sources) {
        if (adj.count(s) && !out.dist.count(s)) {
            out.dist[s] = 0.0; out.parent[s] = ""; out.parent_line[s] = ""; out.root[s] = s;
            heap.push({0.0, s});
        }
    }
    std::set<std::string> done;
    while (!heap.empty()) {
        const auto [d, u] = heap.top(); heap.pop();
        if (done.count(u)) continue;
        done.insert(u);
        for (const auto& edge : adj.at(u)) {
            const auto& v = edge.first;
            const double nd = d + weight(edge.second);
            if (!out.dist.count(v) || nd < out.dist.at(v)) {
                out.dist[v] = nd;
                out.parent[v] = u;
                out.parent_line[v] = edge.second;
                out.root[v] = out.root.at(u);
                heap.push({nd, v});
            }
        }
    }
    return out;
}

struct AllPairs {
    std::vector<std::string> nodes;
    std::map<std::string, std::size_t> index;
    std::vector<std::vector<double>> dist;
    std::vector<std::vector<int>> pred;
    std::map<std::pair<int, int>, std::string> edge_line;

    double distance(const std::string& a, const std::string& b) const { return dist[index.at(a)][index.at(b)]; }
    std::vector<std::string> path(const std::string& a, const std::string& b) const {
        int i = static_cast<int>(index.at(a)), j = static_cast<int>(index.at(b));
        if (dist[i][j] == INF) return {};
        std::vector<int> out{j};
        while (j != i) { j = pred[i][j]; out.push_back(j); }
        std::reverse(out.begin(), out.end());
        std::vector<std::string> result;
        for (int x : out) result.push_back(nodes[static_cast<std::size_t>(x)]);
        return result;
    }
};

AllPairs floyd_warshall(const std::vector<std::string>& nodes,
                        const std::vector<std::tuple<std::string, std::string, double, std::string>>& edges) {
    AllPairs out;
    out.nodes = nodes;
    for (std::size_t i = 0; i < nodes.size(); ++i) out.index[nodes[i]] = i;
    const std::size_t n = nodes.size();
    out.dist.assign(n, std::vector<double>(n, INF));
    out.pred.assign(n, std::vector<int>(n, -1));
    for (std::size_t i = 0; i < n; ++i) { out.dist[i][i] = 0.0; out.pred[i][i] = static_cast<int>(i); }
    for (const auto& edge : edges) {
        const auto& a = std::get<0>(edge); const auto& b = std::get<1>(edge);
        const double w = std::get<2>(edge); const auto& id = std::get<3>(edge);
        const int i = static_cast<int>(out.index.at(a)), j = static_cast<int>(out.index.at(b));
        if (w < out.dist[i][j]) {
            out.dist[i][j] = out.dist[j][i] = w;
            out.pred[i][j] = i; out.pred[j][i] = j;
            out.edge_line[{i, j}] = id; out.edge_line[{j, i}] = id;
        }
    }
    for (std::size_t k = 0; k < n; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            const double dik = out.dist[i][k];
            if (dik == INF) continue;
            for (std::size_t j = 0; j < n; ++j) {
                const double nd = dik + out.dist[k][j];
                if (nd < out.dist[i][j]) {
                    out.dist[i][j] = nd;
                    out.pred[i][j] = out.pred[k][j];
                }
            }
        }
    }
    return out;
}

std::string choose_algorithm(std::size_t sources, std::size_t nodes, std::size_t edges) {
    const double dijkstra_cost = static_cast<double>(sources) * static_cast<double>(nodes + edges) *
                                 std::max(1.0, std::log2(static_cast<double>(std::max<std::size_t>(nodes, 2))));
    return dijkstra_cost <= static_cast<double>(nodes * nodes * nodes) ? "dijkstra" : "floyd-warshall";
}

ShortestPaths shortest_paths_from_sources(
    const std::map<std::string, std::vector<std::pair<std::string, std::string>>>& adj,
    const std::vector<std::string>& all_nodes, const std::vector<std::string>& sources,
    const std::function<double(const std::string&)>& weight, const std::string& requested = "") {
    std::vector<std::string> valid;
    for (const auto& s : sources) if (adj.count(s)) valid.push_back(s);
    std::size_t directed = 0;
    for (const auto& p : adj) directed += p.second.size();
    const std::size_t edges = directed / 2;
    const std::string algorithm = requested.empty() ? choose_algorithm(valid.size(), all_nodes.size(), edges) : requested;
    if (algorithm == "dijkstra") return dijkstra(adj, valid, weight);

    const std::string vs = "__VS__";
    std::vector<std::tuple<std::string, std::string, double, std::string>> edges_all;
    for (const auto& s : valid) edges_all.push_back({vs, s, 0.0, ""});
    std::set<std::string> seen;
    for (const auto& p : adj) for (const auto& edge : p.second) {
        if (!seen.count(edge.second)) {
            seen.insert(edge.second);
            edges_all.push_back({p.first, edge.first, weight(edge.second), edge.second});
        }
    }
    std::vector<std::string> nodes = all_nodes;
    nodes.push_back(vs);
    const AllPairs ap = floyd_warshall(nodes, edges_all);
    const int origin = static_cast<int>(ap.index.at(vs));
    ShortestPaths out;
    out.algorithm = "floyd-warshall";
    for (const auto& v : all_nodes) {
        const int j = static_cast<int>(ap.index.at(v));
        const double d = ap.dist[origin][j];
        if (d == INF) continue;
        const int p = ap.pred[origin][j];
        out.dist[v] = d;
        if (p == origin) { out.parent[v] = ""; out.parent_line[v] = ""; }
        else {
            out.parent[v] = nodes[static_cast<std::size_t>(p)];
            auto it = ap.edge_line.find({p, j});
            out.parent_line[v] = it == ap.edge_line.end() ? "" : it->second;
        }
    }
    for (const auto& p : out.dist) {
        std::string r = p.first;
        while (out.parent.count(r) && !out.parent.at(r).empty()) r = out.parent.at(r);
        out.root[p.first] = r;
    }
    return out;
}

struct BatteryModel {
    double energy_mwh;
    double charge_mw;
    double discharge_mw;
    double efficiency_charge = 0.95;
    double efficiency_discharge = 0.95;
};

struct BatterySchedule {
    std::vector<double> soc_mwh;
    std::vector<double> power_mw;
    double cost = 0.0;
    int levels = 0;
    int transitions = 0;
};

double hourly_cost(double net_mw, double price, double export_price) {
    return price * std::max(net_mw, 0.0) - export_price * std::max(-net_mw, 0.0);
}

double grid_power(double delta_mwh, const BatteryModel& model) {
    if (delta_mwh >= 0.0) return delta_mwh / model.efficiency_charge;
    return delta_mwh * model.efficiency_discharge;
}

BatterySchedule dp_schedule(const std::vector<double>& net_demand, const std::vector<double>& price,
                            const std::vector<double>& export_price, const BatteryModel& model,
                            const std::vector<double>& reserve_mwh, double initial_mwh,
                            std::optional<double> final_min_mwh = std::nullopt,
                            double step_mwh = 0.25, double degradation = 2.0) {
    const int T = static_cast<int>(net_demand.size());
    const int L = static_cast<int>(iround(model.energy_mwh / step_mwh));
    const int up = static_cast<int>(std::floor(model.charge_mw * model.efficiency_charge / step_mwh + 1e-9));
    const int down = static_cast<int>(std::floor(model.discharge_mw / model.efficiency_discharge / step_mwh + 1e-9));
    std::vector<int> floor_level(static_cast<std::size_t>(T + 1));
    for (int t = 0; t <= T; ++t) {
        floor_level[static_cast<std::size_t>(t)] = std::min(
            L, static_cast<int>(std::ceil(reserve_mwh[static_cast<std::size_t>(t)] / step_mwh - 1e-9)));
    }
    const int start = static_cast<int>(iround(initial_mwh / step_mwh));
    const int final_min = final_min_mwh ? static_cast<int>(std::ceil(*final_min_mwh / step_mwh - 1e-9)) : start;
    std::vector<std::vector<double>> value(static_cast<std::size_t>(T + 1), std::vector<double>(static_cast<std::size_t>(L + 1), INF));
    std::vector<std::vector<int>> choice(static_cast<std::size_t>(T), std::vector<int>(static_cast<std::size_t>(L + 1), -1));
    for (int l = std::max(final_min, floor_level[static_cast<std::size_t>(T)]); l <= L; ++l)
        value[static_cast<std::size_t>(T)][static_cast<std::size_t>(l)] = 0.0;
    int transitions = 0;
    for (int t = T - 1; t >= 0; --t) {
        const auto& next = value[static_cast<std::size_t>(t + 1)];
        for (int l = floor_level[static_cast<std::size_t>(t)]; l <= L; ++l) {
            double best = INF;
            int arg = -1;
            const int lo = std::max(floor_level[static_cast<std::size_t>(t + 1)], l - down);
            const int hi = std::min(L, l + up);
            for (int m = lo; m <= hi; ++m) {
                if (next[static_cast<std::size_t>(m)] == INF) continue;
                ++transitions;
                const double delta = (m - l) * step_mwh;
                const double cost = hourly_cost(net_demand[static_cast<std::size_t>(t)] + grid_power(delta, model),
                                                price[static_cast<std::size_t>(t)], export_price[static_cast<std::size_t>(t)]) +
                                    degradation * std::abs(delta) + next[static_cast<std::size_t>(m)];
                if (cost < best - 1e-9) { best = cost; arg = m; }
            }
            value[static_cast<std::size_t>(t)][static_cast<std::size_t>(l)] = best;
            choice[static_cast<std::size_t>(t)][static_cast<std::size_t>(l)] = arg;
        }
    }
    if (start < floor_level[0] || value[0][static_cast<std::size_t>(start)] == INF)
        throw std::runtime_error("no schedule satisfies the reserve and rate limits");
    BatterySchedule out;
    int level = start;
    out.soc_mwh.push_back(start * step_mwh);
    for (int t = 0; t < T; ++t) {
        const int next = choice[static_cast<std::size_t>(t)][static_cast<std::size_t>(level)];
        const double delta = (next - level) * step_mwh;
        out.power_mw.push_back(grid_power(delta, model));
        out.soc_mwh.push_back(next * step_mwh);
        level = next;
    }
    out.cost = value[0][static_cast<std::size_t>(start)];
    out.levels = L + 1;
    out.transitions = transitions;
    return out;
}

BatterySchedule threshold_schedule(const std::vector<double>& net_demand, const std::vector<double>& price,
                                   const std::vector<double>& export_price, const BatteryModel& model,
                                   const std::vector<double>& floor_mwh, double initial_mwh,
                                   double low_quantile = 0.3, double high_quantile = 0.7,
                                   double degradation = 2.0) {
    std::vector<double> ordered = price;
    std::sort(ordered.begin(), ordered.end());
    const std::size_t lo_index = static_cast<std::size_t>(low_quantile * static_cast<double>(ordered.size() - 1));
    const std::size_t hi_index = static_cast<std::size_t>(std::ceil(high_quantile * static_cast<double>(ordered.size() - 1)));
    const double lo = ordered[lo_index], hi = ordered[hi_index];
    BatterySchedule out;
    out.soc_mwh.push_back(initial_mwh);
    double e = initial_mwh;
    for (std::size_t t = 0; t < net_demand.size(); ++t) {
        double delta = 0.0;
        if (net_demand[t] < 0.0 || price[t] <= lo)
            delta = std::min(model.charge_mw * model.efficiency_charge, model.energy_mwh - e);
        else if (price[t] >= hi)
            delta = -std::min(model.discharge_mw / model.efficiency_discharge,
                              std::max(0.0, e - floor_mwh[t + 1]));
        const double p = grid_power(delta, model);
        out.cost += hourly_cost(net_demand[t] + p, price[t], export_price[t]) + degradation * std::abs(delta);
        e += delta;
        out.power_mw.push_back(p);
        out.soc_mwh.push_back(e);
    }
    const double shortfall = std::max(0.0, initial_mwh - e);
    out.cost += shortfall / model.efficiency_charge * *std::min_element(price.begin(), price.end()) +
                degradation * shortfall;
    return out;
}

struct FleetPlan {
    std::string policy;
    std::map<std::string, BatterySchedule> schedules;
    double cost = 0.0;
    int rounds = 0;

    double soc(const std::string& id, int hour) const { return schedules.at(id).soc_mwh.at(static_cast<std::size_t>(hour)); }
};

BatteryModel model_of(const BatterySpec& spec) {
    return {spec.energy_mwh, spec.charge_mw, spec.discharge_mw, spec.efficiency_charge, spec.efficiency_discharge};
}

std::vector<double> net_demand(const Grid& g) {
    std::vector<double> out;
    for (int h = 0; h < HOURS; ++h) out.push_back(g.total_demand_mw(h) - g.renewable_mw(h));
    return out;
}

double system_cost(const Grid& g, const std::map<std::string, BatterySchedule>& schedules) {
    const auto net = net_demand(g);
    double cost = 0.0;
    for (int t = 0; t < HOURS; ++t) {
        double p = 0.0;
        for (const auto& item : schedules) p += item.second.power_mw[static_cast<std::size_t>(t)];
        cost += hourly_cost(net[static_cast<std::size_t>(t)] + p, g.price[static_cast<std::size_t>(t)],
                            g.export_price[static_cast<std::size_t>(t)]);
    }
    for (const auto& item : schedules) {
        const auto& spec = *g.node(item.first).battery;
        const auto& s = item.second;
        for (int t = 0; t < HOURS; ++t)
            cost += 2.0 * std::abs(s.soc_mwh[static_cast<std::size_t>(t + 1)] - s.soc_mwh[static_cast<std::size_t>(t)]);
        const double shortfall = std::max(0.0, s.soc_mwh.front() - s.soc_mwh.back());
        cost += shortfall / spec.efficiency_charge * *std::min_element(g.price.begin(), g.price.end()) + 2.0 * shortfall;
    }
    return cost;
}

std::map<std::string, FleetPlan> make_plans(const Grid& g) {
    std::map<std::string, FleetPlan> out;
    std::map<std::string, BatterySchedule> idle;
    for (const auto& b : g.batteries()) {
        const auto& spec = *g.node(b).battery;
        const double e = spec.initial_soc * spec.energy_mwh;
        idle[b] = BatterySchedule{std::vector<double>(HOURS + 1, e), std::vector<double>(HOURS, 0.0), 0.0, 0, 0};
    }
    out["idle"] = {"idle", idle, system_cost(g, idle), 0};

    const auto net = net_demand(g);
    std::map<std::string, BatterySchedule> threshold;
    for (const auto& b : g.batteries()) {
        const auto& spec = *g.node(b).battery;
        threshold[b] = threshold_schedule(net, g.price, g.export_price, model_of(spec), g.reserve_mwh(b, false),
                                           spec.initial_soc * spec.energy_mwh, 0.3, 0.7, 2.0);
    }
    out["threshold"] = {"threshold", threshold, system_cost(g, threshold), 0};

    auto make_dp = [&](bool with_risk) {
        std::map<std::string, std::vector<double>> power;
        std::map<std::string, BatterySchedule> schedules;
        for (const auto& b : g.batteries()) power[b] = std::vector<double>(HOURS, 0.0);
        int rounds = 0;
        for (rounds = 1; rounds <= 6; ++rounds) {
            bool changed = false;
            for (const auto& b : g.batteries()) {
                std::vector<double> others;
                for (int t = 0; t < HOURS; ++t) {
                    double x = net[static_cast<std::size_t>(t)];
                    for (const auto& c : g.batteries()) if (c != b) x += power[c][static_cast<std::size_t>(t)];
                    others.push_back(x);
                }
                const auto& spec = *g.node(b).battery;
                BatterySchedule s = dp_schedule(others, g.price, g.export_price, model_of(spec),
                                                g.reserve_mwh(b, with_risk), spec.initial_soc * spec.energy_mwh,
                                                std::nullopt, 0.25, 2.0);
                if (s.power_mw != power[b]) changed = true;
                power[b] = s.power_mw;
                schedules[b] = std::move(s);
            }
            if (!changed) break;
        }
        return FleetPlan{with_risk ? "dp" : "dp_no_reserve", schedules, system_cost(g, schedules), rounds};
    };
    out["dp_no_reserve"] = make_dp(false);
    out["dp"] = make_dp(true);
    return out;
}

const std::map<std::string, FleetPlan>& plans() {
    static const std::map<std::string, FleetPlan> p = make_plans(city());
    return p;
}

struct EngineConfig {
    std::string shedding = "mincut";
    bool exchange = true;
    std::string battery_policy = "dp";
    double switch_seconds = 30.0;
    double near_limit = 0.90;
    int max_exchanges = 8;
    double backup_hours = 2.0;
};

struct FlowState {
    FlowNetwork net;
    std::map<std::string, int> line_arc;
    std::map<std::string, int> supply_arc;
    std::map<std::string, int> demand_arc;
    long long total = 0;

    long long value() const { return net.value(SOURCE); }
    long long deficit() const { return total - value(); }
    long long line_flow_kw(const std::string& id) const { return net.flow.at(static_cast<std::size_t>(line_arc.at(id))); }
    long long supplied_kw(const std::string& id) const { return net.flow.at(static_cast<std::size_t>(supply_arc.at(id))); }
    long long delivered_kw(const std::string& id) const {
        return demand_arc.count(id) ? net.flow.at(static_cast<std::size_t>(demand_arc.at(id))) : 0;
    }
};

struct Engine {
    const Grid& g;
    const FleetPlan& plan;
    EngineConfig cfg;
    std::set<std::string> normal_lines;
    std::map<std::string, int> priorities;

    Engine(const Grid& grid, const FleetPlan& fleet, EngineConfig config)
        : g(grid), plan(fleet), cfg(std::move(config)) {
        std::vector<Line> all = g.lines;
        RadialForest normal = kruskal_radial(node_ids(), all, g.plants());
        normal_lines.insert(normal.lines.begin(), normal.lines.end());
        for (const auto& id : g.loads()) priorities[id] = g.node(id).priority;
    }

    std::vector<std::string> node_ids() const {
        std::vector<std::string> out;
        for (const auto& n : g.nodes) out.push_back(n.id);
        return out;
    }
    double battery_available_mw(const std::string& id, int hour) const {
        const auto& spec = *g.node(id).battery;
        const double energy = plan.soc(id, hour) * spec.efficiency_discharge;
        return std::min(spec.discharge_mw, energy / cfg.backup_hours);
    }
    double supply_mw(const std::string& id, int hour) const {
        return g.node(id).kind == Kind::Battery ? battery_available_mw(id, hour) : g.generation_mw(id, hour);
    }

    FlowState network(const std::set<std::string>& lines, const std::vector<std::pair<std::string, long long>>& demand) const {
        FlowState state;
        state.net.node(SOURCE); state.net.node(SINK);
        std::vector<std::string> sorted(lines.begin(), lines.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& id : sorted) {
            const auto& e = g.line(id);
            state.line_arc[id] = state.net.add_edge(e.a, e.b, kw(e.capacity_mw), kw(e.capacity_mw));
        }
        std::vector<std::string> sources = g.renewables();
        auto p = g.plants(); sources.insert(sources.end(), p.begin(), p.end());
        auto b = g.batteries(); sources.insert(sources.end(), b.begin(), b.end());
        for (const auto& s : sources) state.supply_arc[s] = state.net.add_edge(SOURCE, s, 0);
        for (const auto& item : demand) state.demand_arc[item.first] = state.net.add_edge(item.first, SINK, item.second);
        for (const auto& item : demand) state.total += item.second;
        return state;
    }

    FlowState serve(const std::set<std::string>& lines, int hour,
                    const std::vector<std::pair<std::string, long long>>& demand) const {
        FlowState state = network(lines, demand);
        std::vector<std::vector<std::string>> groups = {g.renewables(), g.plants(), g.batteries()};
        for (const auto& group : groups) {
            for (const auto& s : group) state.net.set_capacity(state.supply_arc.at(s), kw(supply_mw(s, hour)));
            state.net.max_flow(SOURCE, SINK);
        }
        return state;
    }

    std::pair<std::vector<long long>, std::optional<std::set<std::string>>>
    served_by_priority(const std::set<std::string>& lines, int hour,
                       const std::vector<std::pair<std::string, long long>>& demand,
                       bool want_cut = false) const {
        std::vector<std::pair<std::string, long long>> zero;
        for (const auto& item : demand) zero.push_back({item.first, 0});
        FlowState state = network(lines, zero);
        std::vector<std::string> sources = g.renewables();
        auto p = g.plants(); sources.insert(sources.end(), p.begin(), p.end());
        auto b = g.batteries(); sources.insert(sources.end(), b.begin(), b.end());
        for (const auto& s : sources) state.net.set_capacity(state.supply_arc.at(s), kw(supply_mw(s, hour)));
        std::vector<long long> served;
        std::optional<std::set<std::string>> cut;
        for (int prio = 1; prio <= 4; ++prio) {
            long long wanted = 0;
            for (const auto& item : demand) if (priorities.at(item.first) == prio) {
                state.net.set_capacity(state.demand_arc.at(item.first), item.second);
                wanted += item.second;
            }
            const long long got = state.net.max_flow(SOURCE, SINK);
            served.push_back(got);
            if (want_cut && !cut && got < wanted) cut = state.net.min_cut_source_side(SOURCE);
        }
        return {served, cut};
    }

    std::vector<long long> class_totals(const std::vector<std::pair<std::string, long long>>& demand) const {
        std::vector<long long> out(4, 0);
        for (const auto& item : demand) out[static_cast<std::size_t>(priorities.at(item.first) - 1)] += item.second;
        return out;
    }

    struct Naive {
        std::map<std::string, double> flows;
        std::vector<std::string> over_lines;
        std::vector<std::string> over_sources;
    };

    Naive naive_flows(const std::set<std::string>& tree, const std::vector<std::string>& roots, int hour) const {
        const auto adj = g.adjacency_map(tree);
        const BFSResult reach = bfs(adj, roots);
        std::map<std::string, double> net;
        for (const auto& n : reach.order) {
            double gen = 0.0;
            if (g.node(n).kind == Kind::Solar || g.node(n).kind == Kind::Wind) gen = g.generation_mw(n, hour);
            net[n] = g.demand_mw(n, hour) - gen;
        }
        std::map<std::string, double> flows;
        for (auto it = reach.order.rbegin(); it != reach.order.rend(); ++it) {
            const auto& n = *it;
            if (!reach.parent_line.at(n).empty()) {
                flows[reach.parent_line.at(n)] = net[n];
                net[reach.parent.at(n)] += net[n];
            }
        }
        Naive out;
        out.flows = std::move(flows);
        for (const auto& item : out.flows)
            if (std::abs(item.second) > g.line(item.first).capacity_mw + 1e-9) out.over_lines.push_back(item.first);
        std::sort(out.over_lines.begin(), out.over_lines.end());
        for (const auto& r : roots) if (net.count(r) && net[r] > supply_mw(r, hour) + 1e-9) out.over_sources.push_back(r);
        std::sort(out.over_sources.begin(), out.over_sources.end());
        return out;
    }

    static std::vector<std::string> cycle(const BFSResult& forest, const std::string& a, const std::string& b) {
        auto up = [&](std::string n) {
            std::vector<std::string> nodes{n}, lines;
            while (!forest.parent.at(n).empty()) {
                lines.push_back(forest.parent_line.at(n));
                n = forest.parent.at(n);
                nodes.push_back(n);
            }
            return std::make_pair(nodes, lines);
        };
        const auto pa = up(a), pb = up(b);
        if (forest.origin.at(a) == forest.origin.at(b)) {
            std::map<std::string, std::size_t> pos;
            for (std::size_t i = 0; i < pa.first.size(); ++i) pos[pa.first[i]] = i;
            for (std::size_t j = 0; j < pb.first.size(); ++j) {
                auto it = pos.find(pb.first[j]);
                if (it != pos.end()) {
                    std::vector<std::string> out(pa.second.begin(), pa.second.begin() + static_cast<std::ptrdiff_t>(it->second));
                    out.insert(out.end(), pb.second.begin(), pb.second.begin() + static_cast<std::ptrdiff_t>(j));
                    return out;
                }
            }
        }
        std::vector<std::string> out = pa.second;
        out.insert(out.end(), pb.second.begin(), pb.second.end());
        return out;
    }

    struct ExchangeResult { std::set<std::string> tree; std::vector<Json> log; };

    ExchangeResult branch_exchange(std::set<std::string> tree, const std::vector<std::string>& roots,
                                   const std::set<std::string>& surviving, int hour,
                                   const std::vector<std::pair<std::string, long long>>& demand) const {
        auto scored = served_by_priority(tree, hour, demand, true);
        std::vector<long long> score = scored.first;
        std::optional<std::set<std::string>> side = scored.second;
        ExchangeResult result{tree, {}};
        for (int attempt = 0; attempt < cfg.max_exchanges; ++attempt) {
            if (!side) break;
            const BFSResult forest = bfs(g.adjacency_map(result.tree), roots);
            bool have_best = false;
            std::vector<long long> best_key;
            double best_resistance = 0.0;
            std::string best_close, best_open;
            std::set<std::string> best_tree;
            std::vector<long long> best_served;
            std::vector<std::string> candidates;
            for (const auto& id : surviving) if (!result.tree.count(id)) candidates.push_back(id);
            std::sort(candidates.begin(), candidates.end());
            for (const auto& id : candidates) {
                const auto& e = g.line(id);
                if ((side->count(e.a) != 0) == (side->count(e.b) != 0)) continue;
                if (!forest.parent.count(e.a) || !forest.parent.count(e.b)) continue;
                for (const auto& open : cycle(forest, e.a, e.b)) {
                    std::set<std::string> trial = result.tree;
                    trial.erase(open); trial.insert(id);
                    const auto served = served_by_priority(trial, hour, demand, false).first;
                    double resistance = 0.0;
                    for (const auto& x : trial) resistance += g.line(x).resistance;
                    const bool better = !have_best || served > best_key ||
                        (served == best_key && -resistance > -best_resistance);
                    if (better) {
                        have_best = true; best_key = served; best_resistance = resistance;
                        best_close = id; best_open = open; best_tree = std::move(trial); best_served = served;
                    }
                }
            }
            if (!have_best || best_served <= score) break;
            Json entry = Json::object();
            entry["close"] = jstr(best_close); entry["open"] = jstr(best_open);
            std::vector<double> before_mw, after_mw;
            for (long long x : score) before_mw.push_back(x / 1000.0);
            for (long long x : best_served) after_mw.push_back(x / 1000.0);
            entry["served_before_mw"] = jnumbers(before_mw, 3);
            entry["served_after_mw"] = jnumbers(after_mw, 3);
            entry["gain_mw"] = jnum(r3(static_cast<double>(std::accumulate(best_served.begin(), best_served.end(), 0LL) -
                                              std::accumulate(score.begin(), score.end(), 0LL)) / 1000.0));
            result.log.push_back(std::move(entry));
            result.tree = std::move(best_tree);
            score = std::move(best_served);
            side = served_by_priority(result.tree, hour, demand, true).second;
        }
        return result;
    }

    std::pair<std::string, int> pick_victim(const std::vector<std::string>& candidates,
                                            const std::map<std::string, int>& blocks,
                                            const std::map<std::string, long long>& full_kw,
                                            long long deficit) const {
        int lowest = 0;
        for (const auto& n : candidates) lowest = std::max(lowest, priorities.at(n));
        std::vector<std::string> pool;
        for (const auto& n : candidates) if (priorities.at(n) == lowest) pool.push_back(n);
        auto served = [&](const std::string& n) -> long long {
            return full_kw.at(n) * blocks.at(n) / g.node(n).blocks;
        };
        std::string victim;
        if (cfg.shedding == "global") {
            for (const auto& n : pool)
                if (victim.empty() || std::make_pair(served(n), n) > std::make_pair(served(victim), victim)) victim = n;
            return {victim, 1};
        }
        std::vector<std::string> fits;
        for (const auto& n : pool) if (served(n) >= deficit) fits.push_back(n);
        if (!fits.empty()) {
            victim = fits.front();
            for (const auto& n : fits)
                if (std::make_pair(served(n), n) < std::make_pair(served(victim), victim)) victim = n;
        } else {
            victim = pool.front();
            for (const auto& n : pool)
                if (std::make_pair(served(n), n) > std::make_pair(served(victim), victim)) victim = n;
        }
        const double block_kw = static_cast<double>(full_kw.at(victim)) / g.node(victim).blocks;
        const int count = block_kw <= 0.0 ? blocks.at(victim) :
            std::min(blocks.at(victim), std::max(1, static_cast<int>(std::ceil(deficit / block_kw))));
        return {victim, count};
    }

    Json bottleneck(const FlowState& state, const std::set<std::string>& side) const {
        Json out = Json::array();
        for (const auto& item : state.line_arc) {
            const auto& e = g.line(item.first);
            if ((side.count(e.a) != 0) != (side.count(e.b) != 0)) {
                Json x = Json::object();
                x["kind"] = jstr("line"); x["id"] = jstr(item.first); x["capacity_mw"] = jnum(e.capacity_mw);
                out.push_back(std::move(x));
            }
        }
        std::vector<std::string> sources = g.renewables();
        auto p = g.plants(); sources.insert(sources.end(), p.begin(), p.end());
        auto b = g.batteries(); sources.insert(sources.end(), b.begin(), b.end());
        for (const auto& s : sources) {
            const int e = state.supply_arc.at(s);
            if (!side.count(s) && state.net.cap[static_cast<std::size_t>(e)] > 0) {
                Json x = Json::object(); x["kind"] = jstr("supply"); x["id"] = jstr(s);
                x["capacity_mw"] = jnum(state.net.cap[static_cast<std::size_t>(e)] / 1000.0);
                out.push_back(std::move(x));
            }
        }
        return out;
    }

    struct ShedResult {
        std::map<std::string, int> blocks;
        std::vector<Json> iterations;
        FlowState state;
    };

    ShedResult shed(const std::set<std::string>& tree, int hour, const std::vector<std::string>& restorable,
                    const std::map<std::string, long long>& full_kw) const {
        std::map<std::string, int> blocks;
        for (const auto& n : restorable) blocks[n] = g.node(n).blocks;
        auto demand = [&]() {
            std::vector<std::pair<std::string, long long>> d;
            for (const auto& n : restorable) if (blocks[n] > 0)
                d.push_back({n, full_kw.at(n) * blocks[n] / g.node(n).blocks});
            return d;
        };
        std::vector<Json> iterations;
        FlowState state = serve(tree, hour, demand());
        while (true) {
            Json record = Json::object();
            record["demand_mw"] = jnum(state.total / 1000.0);
            record["flow_mw"] = jnum(state.value() / 1000.0);
            record["deficit_mw"] = jnum(state.deficit() / 1000.0);
            if (state.deficit() <= 0 || state.demand_arc.empty()) {
                record["action"] = jstr("feasible");
                iterations.push_back(std::move(record));
                break;
            }
            std::vector<std::string> candidates;
            for (const auto& item : state.demand_arc) candidates.push_back(item.first);
            if (cfg.shedding == "mincut") {
                const auto side = state.net.min_cut_source_side(SOURCE);
                std::vector<std::string> filtered;
                for (const auto& n : candidates) if (!side.count(n)) filtered.push_back(n);
                candidates = std::move(filtered);
                record["bottleneck"] = bottleneck(state, side);
            }
            auto victim = pick_victim(candidates, blocks, full_kw, state.deficit());
            const long long before = full_kw.at(victim.first) * blocks.at(victim.first) / g.node(victim.first).blocks;
            blocks[victim.first] -= victim.second;
            const long long after = full_kw.at(victim.first) * blocks.at(victim.first) / g.node(victim.first).blocks;
            record["action"] = jstr("shed"); record["load"] = jstr(victim.first); record["blocks"] = jint(victim.second);
            record["shed_mw"] = jnum((before - after) / 1000.0);
            iterations.push_back(std::move(record));
            state = serve(tree, hour, demand());
        }
        if (cfg.shedding == "mincut") {
            std::vector<std::string> order;
            for (const auto& n : restorable) if (blocks[n] < g.node(n).blocks) order.push_back(n);
            std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
                return std::make_tuple(priorities.at(a), -full_kw.at(a), a) <
                       std::make_tuple(priorities.at(b), -full_kw.at(b), b);
            });
            std::vector<std::string> added;
            for (const auto& n : order) {
                while (blocks[n] < g.node(n).blocks) {
                    ++blocks[n];
                    FlowState trial = serve(tree, hour, demand());
                    if (trial.deficit() > 0) { --blocks[n]; break; }
                    added.push_back(n);
                    state = std::move(trial);
                }
            }
            if (!added.empty()) {
                Json record = Json::object();
                record["demand_mw"] = jnum(state.total / 1000.0); record["flow_mw"] = jnum(state.value() / 1000.0);
                record["deficit_mw"] = jnum(state.deficit() / 1000.0); record["action"] = jstr("added back");
                std::set<std::string> uniq(added.begin(), added.end());
                std::vector<std::string> sorted(uniq.begin(), uniq.end());
                record["loads"] = jstrings(sorted); record["blocks"] = jint(static_cast<long long>(added.size()));
                iterations.push_back(std::move(record));
            }
        }
        return {std::move(blocks), std::move(iterations), std::move(state)};
    }

    struct SwitchResult {
        std::vector<Json> ops;
        std::map<std::string, double> restored_at;
        std::set<std::string> interrupted;
        std::vector<Json> held_off;
        std::set<std::string> closed;
        std::set<std::string> opened;
    };

    SwitchResult switching_plan(std::set<std::string> live, const std::set<std::string>& tree,
                                const std::vector<std::string>& islands, const ShortestPaths& sp,
                                const std::map<std::string, int>& blocks,
                                const std::set<std::string>& energized_before) const {
        const std::set<std::string> original_live = live;
        std::set<std::string> closed = live;
        const auto plant_ids = g.plants();
        std::set<std::string> roots(plant_ids.begin(), plant_ids.end());
        std::vector<Json> ops;
        std::map<std::string, double> restored_at;
        const auto initial_bfs = bfs(g.adjacency_map(closed), plant_ids);
        std::set<std::string> powered(initial_bfs.order.begin(), initial_bfs.order.end());
        std::set<std::string> down;
        for (const auto& n : g.loads()) if (energized_before.count(n) && !powered.count(n)) down.insert(n);
        std::set<std::string> interrupted = down;

        auto apply = [&](Json op) {
            op["t_s"] = jnum(static_cast<double>((ops.size() + 1) * cfg.switch_seconds));
            ops.push_back(op);
            std::set<std::string> root_vec = roots;
            std::vector<std::string> root_list(root_vec.begin(), root_vec.end());
            const auto now_bfs = bfs(g.adjacency_map(closed), root_list);
            std::set<std::string> now(now_bfs.order.begin(), now_bfs.order.end());
            for (const auto& n : g.loads()) {
                if (powered.count(n) && !now.count(n)) {
                    down.insert(n); interrupted.insert(n); restored_at.erase(n);
                } else if (down.count(n) && now.count(n) && blocks.count(n) && blocks.at(n) > 0) {
                    down.erase(n); restored_at[n] = op["t_s"].number();
                }
            }
            powered = std::move(now);
        };

        std::vector<std::string> block_order;
        for (const auto& item : blocks) block_order.push_back(item.first);
        std::sort(block_order.begin(), block_order.end(), [&](const std::string& a, const std::string& b) {
            return std::make_tuple(-priorities.at(a), a) < std::make_tuple(-priorities.at(b), b);
        });
        std::vector<Json> held_off;
        for (const auto& n : block_order) {
            const int missing = g.node(n).blocks - blocks.at(n);
            if (missing <= 0) continue;
            if (powered.count(n)) {
                Json op = Json::object(); op["op"] = jstr("shed"); op["target"] = jstr(n); op["blocks"] = jint(missing);
                apply(std::move(op));
            } else {
                Json item = Json::object(); item["target"] = jstr(n); item["blocks"] = jint(missing);
                held_off.push_back(std::move(item));
            }
        }
        for (const auto& b : islands) {
            roots.insert(b);
            Json op = Json::object(); op["op"] = jstr("start island"); op["target"] = jstr(b);
            apply(std::move(op));
        }
        for (const auto& id : original_live) if (!tree.count(id)) {
            closed.erase(id);
            Json op = Json::object(); op["op"] = jstr("open"); op["line"] = jstr(id);
            apply(std::move(op));
        }
        std::set<std::string> pending;
        for (const auto& id : tree) if (!closed.count(id)) pending.insert(id);
        std::vector<std::string> targets;
        for (const auto& n : g.loads()) if (blocks.count(n) && blocks.at(n) > 0 && sp.dist.count(n)) targets.push_back(n);
        std::sort(targets.begin(), targets.end(), [&](const std::string& a, const std::string& b) {
            return std::tie(priorities.at(a), sp.dist.at(a), a) < std::tie(priorities.at(b), sp.dist.at(b), b);
        });
        for (const auto& n : targets) for (const auto& id : sp.lines_to(n)) if (pending.count(id)) {
            pending.erase(id); closed.insert(id);
            Json op = Json::object(); op["op"] = jstr("close"); op["line"] = jstr(id); op["for"] = jstr(n);
            apply(std::move(op));
        }
        for (const auto& id : pending) {
            closed.insert(id);
            Json op = Json::object(); op["op"] = jstr("close"); op["line"] = jstr(id);
            apply(std::move(op));
        }
        std::vector<std::string> opened;
        for (const auto& id : original_live) if (!tree.count(id)) opened.push_back(id);
        std::vector<std::string> closed_new;
        for (const auto& id : tree) if (!original_live.count(id)) closed_new.push_back(id);
        (void)closed;
        return {std::move(ops), std::move(restored_at), std::move(interrupted), std::move(held_off),
                std::set<std::string>(closed_new.begin(), closed_new.end()),
                std::set<std::string>(opened.begin(), opened.end())};
    }

    static std::set<std::string> reached_set(const BFSResult& result) {
        return std::set<std::string>(result.order.begin(), result.order.end());
    }
    static std::vector<std::string> sorted_ids(const std::set<std::string>& ids) {
        return std::vector<std::string>(ids.begin(), ids.end());
    }
    static Json priority_values(const std::map<int, double>& values) {
        Json out = Json::object();
        for (int p = 1; p <= 4; ++p) out[std::to_string(p)] = jnum(r3(values.count(p) ? values.at(p) : 0.0));
        return out;
    }
    static Json priority_counts(const std::map<int, std::vector<double>>& values) {
        Json out = Json::object();
        for (int p = 1; p <= 4; ++p) {
            Json x = Json::object();
            auto it = values.find(p);
            const auto& arr = it == values.end() ? std::vector<double>{} : it->second;
            x["count"] = jint(static_cast<long long>(arr.size()));
            if (arr.empty()) {
                x["max"] = jnull(); x["mean"] = jnull();
            } else {
                x["max"] = jnum(*std::max_element(arr.begin(), arr.end()));
                x["mean"] = jnum(r1(std::accumulate(arr.begin(), arr.end(), 0.0) / arr.size()));
            }
            out[std::to_string(p)] = std::move(x);
        }
        return out;
    }

    static double r1(double x) { return rround(x, 1); }
    static double r2(double x) { return rround(x, 2); }

    Json report(int hour, const std::vector<std::string>& faults, const std::set<std::string>& active,
                const std::set<std::string>& live, const std::set<std::string>& surviving,
                const std::vector<std::string>& deenergized, const std::vector<std::string>& isolated,
                const std::vector<KruskalStep>& steps, const std::set<std::string>& mst,
                const std::set<std::string>& tree, const std::vector<std::string>& islands,
                const std::vector<std::string>& roots, const std::vector<std::string>& restorable,
                const std::vector<std::string>& unrestorable, const std::map<std::string, long long>& full_kw,
                const Naive& naive, const std::vector<Json>& exchanges,
                const std::map<std::string, int>& blocks, const std::vector<Json>& iterations,
                const FlowState& state, const ShortestPaths& sp, const SwitchResult& route,
                const std::vector<long long>& mesh_bound, const AllPairs& mesh, int loops, int multi_root,
                const std::map<std::string, double>& timings, double decision_ms,
                const BFSResult& before, const BFSResult& after) const {
        (void)surviving;
        (void)restorable;
        Json lines = Json::object();
        double losses_kw = 0.0, max_loading = 0.0;
        std::vector<std::string> near, naive_over = naive.over_lines;
        int violations = 0;
        std::set<std::string> closed_new, opened;
        for (const auto& id : tree) if (!live.count(id)) closed_new.insert(id);
        for (const auto& id : live) if (!tree.count(id)) opened.insert(id);
        for (const auto& e : g.lines) {
            double flow = 0.0;
            long long flow_kw = 0;
            if (tree.count(e.id)) { flow_kw = state.line_flow_kw(e.id); flow = flow_kw / 1000.0; }
            const double loading = e.capacity_mw ? std::abs(flow) / e.capacity_mw : 0.0;
            if (tree.count(e.id)) {
                losses_kw += e.resistance * flow * flow / (g.nominal_kv * g.nominal_kv) * 1000.0;
                max_loading = std::max(max_loading, loading);
                if (loading >= cfg.near_limit - 1e-9) near.push_back(e.id);
                if (std::abs(flow_kw) > kw(e.capacity_mw)) ++violations;
            }
            Json item = Json::object();
            item["status"] = jstr(std::find(faults.begin(), faults.end(), e.id) != faults.end() ? "faulted" :
                                     (tree.count(e.id) ? "active" : "open"));
            if (closed_new.count(e.id)) item["change"] = jstr("closed");
            else if (opened.count(e.id)) item["change"] = jstr("opened");
            else item["change"] = jnull();
            item["was_active"] = jbool(active.count(e.id) != 0);
            item["in_mst"] = jbool(mst.count(e.id) != 0);
            item["flow_mw"] = jnum(r3(flow)); item["loading"] = jnum(r4(loading));
            auto nit = naive.flows.find(e.id);
            if (nit == naive.flows.end()) item["naive_flow_mw"] = jnull();
            else item["naive_flow_mw"] = jnum(r3(nit->second));
            item["naive_overload"] = jbool(std::find(naive_over.begin(), naive_over.end(), e.id) != naive_over.end());
            lines[e.id] = std::move(item);
        }

        const std::set<std::string> deenergized_set(deenergized.begin(), deenergized.end());
        const std::set<std::string> isolated_set(isolated.begin(), isolated.end());
        Json nodes = Json::object();
        std::map<std::string, double> served_by_node;
        for (const auto& n : g.nodes) {
            Json info = Json::object();
            info["energized_before"] = jbool(before.reached(n.id));
            info["deenergized"] = jbool(deenergized_set.count(n.id) != 0);
            info["isolated"] = jbool(isolated_set.count(n.id) != 0);
            if (sp.root.count(n.id)) info["root"] = jstr(sp.root.at(n.id)); else info["root"] = jnull();
            if (n.kind == Kind::Load) {
                const auto bit = blocks.find(n.id);
                const int block_count = bit == blocks.end() ? 0 : bit->second;
                const long long served_kw = bit == blocks.end() ? 0 : full_kw.at(n.id) * block_count / n.blocks;
                std::string status;
                if (std::find(unrestorable.begin(), unrestorable.end(), n.id) != unrestorable.end()) status = "unrestorable";
                else if (block_count == 0) status = "shed";
                else if (block_count < n.blocks) status = "partial";
                else status = "served";
                double mesh_r = INF;
                for (const auto& p : g.plants()) mesh_r = std::min(mesh_r, mesh.distance(p, n.id));
                const auto tree_it = sp.dist.find(n.id);
                const bool has_tree = tree_it != sp.dist.end();
                info["status"] = jstr(status); info["priority"] = jint(n.priority);
                info["demand_mw"] = jnum(full_kw.at(n.id) / 1000.0); info["served_mw"] = jnum(served_kw / 1000.0);
                info["blocks"] = jint(n.blocks); info["blocks_served"] = jint(block_count);
                info["interrupted"] = jbool(route.interrupted.count(n.id) != 0);
                if (block_count == 0) info["restored_s"] = jnull();
                else if (route.restored_at.count(n.id)) info["restored_s"] = jnum(route.restored_at.at(n.id));
                else if (route.interrupted.count(n.id)) info["restored_s"] = jnull();
                else info["restored_s"] = jnum(0.0);
                info["path"] = jstrings(has_tree ? sp.path_to(n.id) : std::vector<std::string>{});
                info["path_lines"] = jstrings(has_tree ? sp.lines_to(n.id) : std::vector<std::string>{});
                info["path_resistance"] = has_tree ? jnum(r4(tree_it->second)) : jnull();
                info["mesh_resistance"] = mesh_r == INF ? jnull() : jnum(r4(mesh_r));
                if (has_tree && mesh_r != INF && mesh_r != 0.0 && sp.root.at(n.id).size() &&
                    g.node(sp.root.at(n.id)).kind == Kind::Plant)
                    info["stretch"] = jnum(r3(tree_it->second / mesh_r));
                else info["stretch"] = jnull();
                served_by_node[n.id] = served_kw / 1000.0;
            } else if (n.kind == Kind::Battery || n.kind == Kind::Plant || n.kind == Kind::Solar || n.kind == Kind::Wind) {
                const double available = supply_mw(n.id, hour);
                info["available_mw"] = jnum(r3(available));
                info["supplied_mw"] = jnum(r3(state.supplied_kw(n.id) / 1000.0));
                if (n.kind == Kind::Battery) {
                    info["soc_mwh"] = jnum(r3(plan.soc(n.id, hour)));
                    info["reserve_mwh"] = jnum(r3(g.reserve_mwh(n.id)[static_cast<std::size_t>(hour)]));
                    info["island_root"] = jbool(std::find(islands.begin(), islands.end(), n.id) != islands.end());
                }
            }
            nodes[n.id] = std::move(info);
        }

        std::map<int, double> demand, served, shed, lost;
        for (int p = 1; p <= 4; ++p) demand[p] = served[p] = shed[p] = lost[p] = 0.0;
        for (const auto& n : g.loads()) {
            const int p = priorities.at(n);
            demand[p] += full_kw.at(n) / 1000.0;
            served[p] += served_by_node[n];
            if (std::find(unrestorable.begin(), unrestorable.end(), n) != unrestorable.end()) lost[p] += full_kw.at(n) / 1000.0;
            else {
                const int nblocks = blocks.at(n);
                const long long exact_served = full_kw.at(n) * nblocks / g.node(n).blocks;
                shed[p] += (full_kw.at(n) - exact_served) / 1000.0;
            }
        }
        for (int p = 1; p <= 4; ++p) { demand[p] = r3(demand[p]); served[p] = r3(served[p]); shed[p] = r3(shed[p]); lost[p] = r3(lost[p]); }

        std::map<int, std::vector<double>> restoration;
        for (const auto& n : g.loads()) if (route.restored_at.count(n)) restoration[priorities.at(n)].push_back(route.restored_at.at(n));
        Json summary = Json::object();
        summary["hour"] = jint(hour); summary["faults"] = jstrings(faults);
        summary["demand_mw"] = priority_values(demand); summary["served_mw"] = priority_values(served);
        summary["shed_mw"] = priority_values(shed); summary["unrestorable_mw"] = priority_values(lost);
        const double total_demand = std::accumulate(demand.begin(), demand.end(), 0.0,
            [](double x, const auto& p) { return x + p.second; });
        const double total_served = std::accumulate(served.begin(), served.end(), 0.0,
            [](double x, const auto& p) { return x + p.second; });
        summary["total_demand_mw"] = jnum(r3(total_demand)); summary["total_served_mw"] = jnum(r3(total_served));
        summary["critical_served_pct"] = jnum(demand[1] ? r2(100.0 * served[1] / demand[1]) : 100.0);
        summary["deenergized_nodes"] = jint(static_cast<long long>(deenergized.size()));
        summary["interrupted_loads"] = jint(static_cast<long long>(route.interrupted.size()));
        summary["restored_loads"] = jint(static_cast<long long>(route.restored_at.size()));
        summary["restoration_s"] = priority_counts(restoration);
        summary["switch_ops"] = jint(static_cast<long long>(route.ops.size()));
        summary["exchanges"] = jint(static_cast<long long>(exchanges.size()));
        long long shed_iterations = 0; for (const auto& it : iterations) if (it["action"].string() == "shed") ++shed_iterations;
        summary["shed_iterations"] = jint(shed_iterations);
        summary["overloads_prevented"] = jint(static_cast<long long>(naive.over_lines.size()));
        summary["source_overloads_prevented"] = jint(static_cast<long long>(naive.over_sources.size()));
        summary["max_loading"] = jnum(r4(max_loading)); summary["near_limit_lines"] = jstrings(near);
        summary["capacity_violations"] = jint(violations); summary["loop_violations"] = jint(loops);
        summary["multi_source_trees"] = jint(multi_root); summary["losses_kw"] = jnum(r2(losses_kw));
        std::vector<double> mesh_mw; for (auto x : mesh_bound) mesh_mw.push_back(x / 1000.0);
        summary["mesh_bound_mw"] = jnumbers(mesh_mw, 3);
        const double bound_total = std::accumulate(mesh_mw.begin(), mesh_mw.end(), 0.0);
        summary["served_vs_bound"] = jnum(bound_total ? r4(total_served / bound_total) : 1.0);
        summary["route_algorithm"] = jstr(sp.algorithm); summary["latency_ms"] = jnum(r3(decision_ms));
        Json timing = Json::object(); for (const auto& item : timings) timing[item.first] = jnum(r3(item.second));
        summary["timings_ms"] = std::move(timing);
        Json policy = Json::object(); policy["shedding"] = jstr(cfg.shedding); policy["exchange"] = jbool(cfg.exchange); policy["battery"] = jstr(plan.policy);
        summary["policy"] = std::move(policy);

        Json supply = Json::object();
        std::vector<std::string> all_sources = g.renewables(); auto pids = g.plants(); all_sources.insert(all_sources.end(), pids.begin(), pids.end()); auto bids = g.batteries(); all_sources.insert(all_sources.end(), bids.begin(), bids.end());
        for (const auto& s : all_sources) supply[s] = jnum(r3(state.supplied_kw(s) / 1000.0));

        Json kruskal = Json::array();
        for (const auto& step : steps) {
            Json x = Json::object(); x["kind"] = jstr(step.kind); x["item"] = jstr(step.item); x["a"] = jstr(step.a); x["b"] = jstr(step.b);
            x["weight"] = step.weight ? jnum(*step.weight) : jnull(); x["accepted"] = jbool(step.accepted); kruskal.push_back(std::move(x));
        }
        Json rebuild = Json::object(); rebuild["rebuilt"] = jbool(!steps.empty()); rebuild["kruskal"] = std::move(kruskal);
        rebuild["mst_lines"] = jstrings(sorted_ids(mst)); rebuild["islands"] = jstrings(islands); rebuild["roots"] = jstrings(roots);
        std::set<std::string> closed_by_mst; for (const auto& id : mst) if (!live.count(id)) closed_by_mst.insert(id);
        rebuild["closed_by_mst"] = jstrings(sorted_ids(closed_by_mst));

        Json verify = Json::object(); verify["naive_overloads"] = jstrings(naive.over_lines); verify["source_overloads"] = jstrings(naive.over_sources);
        Json exarr = Json::array(); for (const auto& x : exchanges) exarr.push_back(x); verify["exchanges"] = std::move(exarr);
        Json itarr = Json::array(); for (const auto& x : iterations) itarr.push_back(x); verify["iterations"] = std::move(itarr);
        verify["supply_mw"] = std::move(supply); verify["final_lines"] = jstrings(sorted_ids(tree));

        Json route_json = Json::object(); route_json["algorithm"] = jstr(sp.algorithm);
        Json ops = Json::array(); for (const auto& x : route.ops) ops.push_back(x); route_json["sequence"] = std::move(ops);
        route_json["held_off"] = [&]() { Json x = Json::array(); for (const auto& y : route.held_off) x.push_back(y); return x; }();
        route_json["closed"] = jstrings(sorted_ids(route.closed)); route_json["opened"] = jstrings(sorted_ids(route.opened));

        Json detect = Json::object(); detect["bfs_order"] = jstrings(after.order); detect["deenergized"] = jstrings(deenergized); detect["isolated"] = jstrings(isolated);
        std::map<int, double> lost_by_priority; for (int p = 1; p <= 4; ++p) lost_by_priority[p] = 0.0;
        for (const auto& n : deenergized) if (g.node(n).kind == Kind::Load)
            lost_by_priority[priorities.at(n)] += full_kw.at(n) / 1000.0;
        for (int p = 1; p <= 4; ++p) lost_by_priority[p] = r3(lost_by_priority[p]);
        detect["lost_mw"] = priority_values(lost_by_priority);

        Json out = Json::object(); out["summary"] = std::move(summary); out["nodes"] = std::move(nodes); out["lines"] = std::move(lines);
        out["detect"] = std::move(detect); out["rebuild"] = std::move(rebuild); out["verify"] = std::move(verify); out["route"] = std::move(route_json);
        return out;
    }

    Json handle_event(int input_hour, const std::vector<std::string>& input_faults) const {
        int hour = input_hour % HOURS;
        if (hour < 0) hour += HOURS;
        std::set<std::string> fault_set;
        for (const auto& id : input_faults) if (g.line_index.count(id)) fault_set.insert(id);
        const std::vector<std::string> faults(fault_set.begin(), fault_set.end());
        const std::set<std::string> active = normal_lines;
        std::set<std::string> surviving;
        for (const auto& e : g.lines) if (!fault_set.count(e.id)) surviving.insert(e.id);
        std::set<std::string> live = active;
        for (const auto& id : fault_set) live.erase(id);
        const auto plants = g.plants();

        const auto detect_start = std::chrono::steady_clock::now();
        const auto before = bfs(g.adjacency_map(active), plants);
        const auto after = bfs(g.adjacency_map(live), plants);
        std::vector<std::string> deenergized;
        for (const auto& n : before.order) if (!after.parent.count(n)) deenergized.push_back(n);
        const auto surviving_reach = bfs(g.adjacency_map(surviving), plants);
        std::vector<std::string> isolated;
        for (const auto& n : g.nodes) if (!surviving_reach.parent.count(n.id)) isolated.push_back(n.id);
        const double detect_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - detect_start).count();

        const auto rebuild_start = std::chrono::steady_clock::now();
        std::vector<KruskalStep> steps;
        std::set<std::string> mst;
        std::vector<std::string> islands;
        if (!deenergized.empty()) {
            std::vector<std::string> backups;
            for (const auto& b : g.batteries()) if (battery_available_mw(b, hour) > 0.0) backups.push_back(b);
            std::vector<Line> surviving_lines;
            for (const auto& id : surviving) surviving_lines.push_back(g.line(id));
            const auto forest = kruskal_radial(node_ids(), surviving_lines, plants, backups);
            mst.insert(forest.lines.begin(), forest.lines.end());
            steps = forest.steps;
            islands = forest.island_roots;
        } else {
            mst = live;
        }
        const auto island_members = bfs(g.adjacency_map(mst), islands);
        std::vector<std::string> useful_islands;
        for (const auto& b : islands) {
            bool useful = false;
            for (const auto& n : g.loads()) if (island_members.origin.count(n) && island_members.origin.at(n) == b) useful = true;
            if (useful) useful_islands.push_back(b);
        }
        islands = std::move(useful_islands);
        const double rebuild_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rebuild_start).count();
        std::vector<std::string> roots = plants;
        roots.insert(roots.end(), islands.begin(), islands.end());

        const auto verify_start = std::chrono::steady_clock::now();
        const auto reach = bfs(g.adjacency_map(mst), roots);
        std::vector<std::string> restorable, unrestorable;
        for (const auto& n : g.loads()) {
            if (reach.parent.count(n)) restorable.push_back(n); else unrestorable.push_back(n);
        }
        std::map<std::string, long long> full_kw;
        for (const auto& n : g.loads()) full_kw[n] = kw(g.demand_mw(n, hour));
        std::vector<std::pair<std::string, long long>> demand;
        for (const auto& n : restorable) demand.push_back({n, full_kw[n]});
        const Naive naive = naive_flows(mst, roots, hour);
        std::set<std::string> tree = mst;
        std::vector<Json> exchanges;
        if (cfg.exchange) {
            const auto ex = branch_exchange(tree, roots, surviving, hour, demand);
            tree = ex.tree; exchanges = ex.log;
        }
        const auto shed_result = shed(tree, hour, restorable, full_kw);
        const double verify_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - verify_start).count();
        const auto route_start = std::chrono::steady_clock::now();
        const auto sp = shortest_paths_from_sources(g.adjacency_map(tree), node_ids(), roots,
                                                    [&](const std::string& id) { return g.line(id).resistance; });
        const auto route = switching_plan(live, tree, islands, sp, shed_result.blocks, reached_set(before));
        const double route_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - route_start).count();
        const auto evaluation_start = std::chrono::steady_clock::now();
        std::vector<std::pair<std::string, long long>> mesh_demand;
        for (const auto& n : g.loads()) mesh_demand.push_back({n, full_kw[n]});
        const std::vector<long long> mesh_bound = served_by_priority(surviving, hour, mesh_demand, false).first;
        std::vector<std::tuple<std::string, std::string, double, std::string>> mesh_edges;
        for (const auto& id : surviving) {
            const auto& e = g.line(id);
            mesh_edges.push_back({e.a, e.b, e.resistance, e.id});
        }
        const auto mesh = floyd_warshall(node_ids(), mesh_edges);
        DisjointSet dsu(node_ids());
        int loops = 0;
        for (const auto& id : tree) if (!dsu.unite(g.line(id).a, g.line(id).b)) ++loops;
        std::map<std::string, std::vector<std::string>> components;
        for (const auto& r : roots) components[dsu.find(r)].push_back(r);
        int multi_root = 0;
        for (const auto& item : components) if (item.second.size() > 1) ++multi_root;
        const double evaluation_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - evaluation_start).count();
        const std::map<std::string, double> timings = {
            {"detect", detect_ms}, {"rebuild", rebuild_ms}, {"verify", verify_ms},
            {"route", route_ms}, {"evaluation", evaluation_ms},
        };
        const double decision_ms = detect_ms + rebuild_ms + verify_ms + route_ms;
        return report(hour, faults, active, live, surviving, deenergized, isolated, steps, mst, tree, islands, roots,
                      restorable, unrestorable, full_kw, naive, exchanges, shed_result.blocks, shed_result.iterations,
                      shed_result.state, sp, route, mesh_bound, mesh, loops, multi_root, timings, decision_ms, before, after);
    }
};

Json node_json(const Node& n) {
    Json out = Json::object();
    out["id"] = jstr(n.id); out["name"] = jstr(n.name); out["kind"] = jstr(kind_name(n.kind));
    out["x"] = jnum(n.x); out["y"] = jnum(n.y); out["capacity_mw"] = jnum(n.capacity_mw);
    out["peak_mw"] = jnum(n.peak_mw); out["priority"] = jint(n.priority); out["category"] = jstr(n.category); out["blocks"] = jint(n.blocks);
    if (n.battery) {
        Json b = Json::object();
        b["energy_mwh"] = jnum(n.battery->energy_mwh); b["charge_mw"] = jnum(n.battery->charge_mw);
        b["discharge_mw"] = jnum(n.battery->discharge_mw); b["efficiency_charge"] = jnum(n.battery->efficiency_charge);
        b["efficiency_discharge"] = jnum(n.battery->efficiency_discharge); b["initial_soc"] = jnum(n.battery->initial_soc);
        b["reserve_floor"] = jnum(n.battery->reserve_floor); b["risk_reserve"] = jnum(n.battery->risk_reserve);
        out["battery"] = std::move(b);
    } else out["battery"] = jnull();
    return out;
}

Json line_json(const Line& e) {
    Json out = Json::object(); out["id"] = jstr(e.id); out["a"] = jstr(e.a); out["b"] = jstr(e.b);
    out["resistance"] = jnum(e.resistance); out["capacity_mw"] = jnum(e.capacity_mw); return out;
}

Json plan_json(const FleetPlan& plan) {
    static const std::map<std::string, std::string> labels = {
        {"idle", "No arbitrage (idle)"}, {"threshold", "Threshold rule"},
        {"dp_no_reserve", "DP, cost only"}, {"dp", "DP with risk reserve"},
    };
    Json out = Json::object(); out["policy"] = jstr(plan.policy); out["label"] = jstr(labels.at(plan.policy));
    out["cost"] = jnum(r2(plan.cost)); out["rounds"] = jint(plan.rounds);
    Json batteries = Json::object();
    for (const auto& item : plan.schedules) {
        const auto& s = item.second;
        Json b = Json::object(); b["soc_mwh"] = jnumbers(s.soc_mwh, 3); b["power_mw"] = jnumbers(s.power_mw, 3);
        b["levels"] = jint(s.levels); b["transitions"] = jint(s.transitions); batteries[item.first] = std::move(b);
    }
    out["batteries"] = std::move(batteries); return out;
}

Json presets_json() {
    struct Preset { const char* id; const char* name; int hour; std::vector<std::string> faults; const char* description; };
    const std::vector<Preset> presets = {
        {"normal-peak", "Normal evening peak", 19, {}, "No faults: the intact radial network at the busiest hour of the day."},
        {"hospital-feeder", "Hospital feeder cut", 10, {"L10"}, "A falling tree cuts the North-Central feeder, blacking out the hospital's area."},
        {"wind-loss", "Wind farm line lost at peak", 19, {"L16"}, "Lightning trips the wind farm's connection during the evening peak."},
        {"west-plant-trip", "Storm trips the West plant", 19, {"L01"}, "The West Gas Plant's only line fails at peak: half the city must be re-fed from the east."},
        {"hospital-island", "Double fault isolates the hospital", 20, {"L11", "T01"}, "Both feeds to the hospital fail; only its own battery can reach it."},
        {"east-substation-fire", "East Substation fire", 18, {"L18", "L19", "L23", "L24", "L27", "T11", "T13", "T14"}, "Every line into the East Substation is lost, cutting the East plant off from the city."},
    };
    Json out = Json::array();
    for (const auto& p : presets) {
        Json x = Json::object(); x["id"] = jstr(p.id); x["name"] = jstr(p.name); x["hour"] = jint(p.hour);
        x["faults"] = jstrings(p.faults); x["description"] = jstr(p.description); out.push_back(std::move(x));
    }
    return out;
}

Json boot_json() {
    const auto& g = city();
    const auto& p = plans();
    EngineConfig cfg;
    Engine engine(g, p.at("dp"), cfg);
    Json out = Json::object(); out["name"] = jstr(g.name); out["nominal_kv"] = jnum(g.nominal_kv);
    Json nodes = Json::array(); for (const auto& n : g.nodes) nodes.push_back(node_json(n)); out["nodes"] = std::move(nodes);
    Json lines = Json::array(); for (const auto& e : g.lines) lines.push_back(line_json(e)); out["lines"] = std::move(lines);
    out["solar_factor"] = jnumbers(g.solar_factor); out["wind_factor"] = jnumbers(g.wind_factor);
    Json demand = Json::object(); for (const auto& item : g.demand_factor) demand[item.first] = jnumbers(item.second); out["demand_factor"] = std::move(demand);
    out["price"] = jnumbers(g.price); out["export_price"] = jnumbers(g.export_price);
    std::vector<long long> risk; for (int h : g.risk_hours) risk.push_back(h); out["risk_hours"] = jint_array(risk);
    Json priority_names = Json::object(); priority_names["1"] = jstr("Critical"); priority_names["2"] = jstr("Essential");
    priority_names["3"] = jstr("Residential"); priority_names["4"] = jstr("Commercial"); out["priority_names"] = std::move(priority_names);
    out["normal_lines"] = jstrings(Engine::sorted_ids(engine.normal_lines)); out["presets"] = presets_json();
    Json profile = Json::object(); std::vector<double> demand_profile, renewable_profile, net_profile;
    const auto nd = net_demand(g);
    for (int h = 0; h < HOURS; ++h) { demand_profile.push_back(r3(g.total_demand_mw(h))); renewable_profile.push_back(r3(g.renewable_mw(h))); net_profile.push_back(r3(nd[static_cast<std::size_t>(h)])); }
    profile["demand_mw"] = jnumbers(demand_profile); profile["renewable_mw"] = jnumbers(renewable_profile); profile["net_demand_mw"] = jnumbers(net_profile);
    double plant_mw = 0.0; for (const auto& id : g.plants()) plant_mw += g.node(id).capacity_mw; profile["plant_mw"] = jnum(plant_mw); out["profiles"] = std::move(profile);
    Json battery_plans = Json::object(); for (const auto& item : p) battery_plans[item.first] = plan_json(item.second); out["battery_plans"] = std::move(battery_plans);
    Json reserves = Json::object(); for (const auto& b : g.batteries()) reserves[b] = jnumbers(g.reserve_mwh(b), 3); out["reserve_mwh"] = std::move(reserves);
    out["battery_policy"] = jstr("dp");
    return out;
}

} // namespace

Json boot() { return boot_json(); }

Json battery_plans() {
    Json out = Json::object();
    for (const auto& item : plans()) out[item.first] = plan_json(item.second);
    return out;
}

Json run_event(int hour, const std::vector<std::string>& faults, const std::string& policy,
               const std::string& battery_policy, bool audit) {
    (void)audit;
    const auto& p = plans();
    const std::string battery = p.count(battery_policy) ? battery_policy : "dp";
    EngineConfig cfg;
    if (policy == "proposal") { cfg.shedding = "global"; cfg.exchange = false; }
    else if (policy == "mincut") { cfg.shedding = "mincut"; cfg.exchange = false; }
    else if (policy == "global-exchange") { cfg.shedding = "global"; cfg.exchange = true; }
    else { cfg.shedding = "mincut"; cfg.exchange = true; }
    cfg.battery_policy = battery;
    Engine engine(city(), p.at(battery), cfg);
    return engine.handle_event(hour, faults);
}

} // namespace sg

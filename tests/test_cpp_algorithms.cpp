// Independent randomized/oracle checks for the native algorithm implementations.
// core.cpp is included deliberately: these algorithms are private implementation
// details, and this test translation unit can still exercise them without making
// them part of the production public API.
#include "../cpp/core.cpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using Adj = std::map<std::string, std::vector<std::pair<std::string, std::string>>>;
using Rng = std::mt19937;

int random_int(Rng& rng, int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(rng);
}

double random_real(Rng& rng, double low, double high) {
    return std::uniform_real_distribution<double>(low, high)(rng);
}

template <typename T>
std::vector<T> sample_without_replacement(Rng& rng, const std::vector<T>& values, std::size_t count) {
    std::vector<T> out = values;
    for (std::size_t j = out.size(); j > 1; --j) {
        const std::size_t pick = static_cast<std::size_t>(rng()) % j;
        std::swap(out[j - 1], out[pick]);
    }
    if (count < out.size()) out.resize(count);
    return out;
}

struct Suite {
    explicit Suite(std::string suite_name) : name(std::move(suite_name)) {}

    std::string name;
    int cases = 0;
    int failed_cases = 0;
    int checks = 0;
    int failures = 0;
    bool case_failed = false;

    void begin_case() {
        ++cases;
        case_failed = false;
    }

    void require(bool condition, const std::string& message) {
        ++checks;
        if (condition) return;
        case_failed = true;
        ++failures;
        if (failures <= 20) std::cerr << "FAIL " << name << ": " << message << '\n';
    }

    void end_case() {
        if (case_failed) ++failed_cases;
    }
};

struct RandomGraph {
    std::vector<std::string> nodes;
    std::vector<std::tuple<std::string, std::string, double, std::string>> edges;
    Adj adjacency;
};

RandomGraph random_graph(Rng& rng, int node_count, int edge_count, bool weighted = true) {
    RandomGraph graph;
    for (int i = 0; i < node_count; ++i) graph.nodes.push_back("n" + std::to_string(i));
    for (const auto& node : graph.nodes) graph.adjacency[node] = {};
    for (int k = 0; k < edge_count; ++k) {
        const int ai = random_int(rng, 0, node_count - 1);
        int bi = random_int(rng, 0, node_count - 1);
        while (bi == ai) bi = random_int(rng, 0, node_count - 1);
        const std::string a = graph.nodes[static_cast<std::size_t>(ai)];
        const std::string b = graph.nodes[static_cast<std::size_t>(bi)];
        const double weight = weighted ? std::round(random_real(rng, 0.1, 5.0) * 1000.0) / 1000.0 : 1.0;
        const std::string id = "e" + std::to_string(k);
        graph.edges.emplace_back(a, b, weight, id);
        graph.adjacency[a].push_back({b, id});
        graph.adjacency[b].push_back({a, id});
    }
    return graph;
}

std::map<std::string, int> bellman_ford_hops(
    const std::vector<std::string>& nodes,
    const std::vector<std::tuple<std::string, std::string, double, std::string>>& edges,
    const std::vector<std::string>& sources) {
    constexpr int inf = std::numeric_limits<int>::max() / 4;
    std::map<std::string, int> hops;
    for (const auto& node : nodes) hops[node] = inf;
    for (const auto& source : sources) hops[source] = 0;
    for (std::size_t pass = 0; pass < nodes.size(); ++pass) {
        bool changed = false;
        for (const auto& edge : edges) {
            const auto& a = std::get<0>(edge);
            const auto& b = std::get<1>(edge);
            if (hops[a] < inf && hops[b] > hops[a] + 1) {
                hops[b] = hops[a] + 1;
                changed = true;
            }
            if (hops[b] < inf && hops[a] > hops[b] + 1) {
                hops[a] = hops[b] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return hops;
}

void test_bfs() {
    Suite suite("BFS");
    Rng rng(1);
    for (int case_index = 0; case_index < 200; ++case_index) {
        suite.begin_case();
        const int node_count = random_int(rng, 2, 12);
        const int edge_count = random_int(rng, 0, 20);
        const RandomGraph graph = random_graph(rng, node_count, edge_count, false);
        const int source_count = random_int(rng, 1, std::min(3, node_count));
        const auto sources = sample_without_replacement(rng, graph.nodes, static_cast<std::size_t>(source_count));
        const sg::BFSResult result = sg::bfs(graph.adjacency, sources);
        const auto hops = bellman_ford_hops(graph.nodes, graph.edges, sources);

        std::set<std::string> expected_reached;
        for (const auto& item : hops) {
            if (item.second < std::numeric_limits<int>::max() / 4) expected_reached.insert(item.first);
        }
        std::set<std::string> actual_reached;
        for (const auto& item : result.parent) actual_reached.insert(item.first);
        suite.require(actual_reached == expected_reached, "reachability differs from unit-weight relaxation");
        for (const auto& node : expected_reached) {
            suite.require(result.hops.at(node) == hops.at(node), "hop count differs for " + node);
            suite.require(static_cast<int>(result.path_to(node).size()) - 1 == hops.at(node),
                          "reconstructed hop path differs for " + node);
        }
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("BFS suite failed");
}

void test_disjoint_set() {
    Suite suite("DSU");
    Rng rng(2);
    for (int case_index = 0; case_index < 100; ++case_index) {
        suite.begin_case();
        const int n = random_int(rng, 1, 15);
        std::vector<std::string> items;
        for (int i = 0; i < n; ++i) items.push_back("v" + std::to_string(i));
        sg::DisjointSet dsu(items);
        std::vector<int> labels(static_cast<std::size_t>(n));
        std::iota(labels.begin(), labels.end(), 0);
        const int operations = random_int(rng, 0, 25);
        for (int operation = 0; operation < operations; ++operation) {
            const int ai = random_int(rng, 0, n - 1);
            const int bi = random_int(rng, 0, n - 1);
            const bool expected_merged = labels[static_cast<std::size_t>(ai)] != labels[static_cast<std::size_t>(bi)];
            const bool merged = dsu.unite(items[static_cast<std::size_t>(ai)], items[static_cast<std::size_t>(bi)]);
            suite.require(merged == expected_merged, "union result differs from naive labels");
            if (expected_merged) {
                const int old = labels[static_cast<std::size_t>(bi)];
                const int next = labels[static_cast<std::size_t>(ai)];
                for (int& label : labels) if (label == old) label = next;
            }
        }
        for (int ai = 0; ai < n; ++ai) {
            for (int bi = 0; bi < n; ++bi) {
                suite.require(dsu.connected(items[static_cast<std::size_t>(ai)], items[static_cast<std::size_t>(bi)]) ==
                                  (labels[static_cast<std::size_t>(ai)] == labels[static_cast<std::size_t>(bi)]),
                              "connected query differs from naive labels");
            }
        }
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("DSU suite failed");
}

struct WeightedEdge {
    std::string a;
    std::string b;
    double weight = 0.0;
};

double brute_force_forest_weight(const std::vector<std::string>& nodes,
                                const std::vector<WeightedEdge>& edges) {
    std::vector<std::string> dsu_nodes = nodes;
    dsu_nodes.push_back("__ROOT__");
    sg::DisjointSet all(dsu_nodes);
    for (const auto& edge : edges) all.unite(edge.a, edge.b);
    std::set<std::string> components;
    for (const auto& node : dsu_nodes) components.insert(all.find(node));
    const std::size_t need = dsu_nodes.size() - components.size();
    if (need > edges.size()) return std::numeric_limits<double>::infinity();

    double best = std::numeric_limits<double>::infinity();
    std::vector<std::size_t> choice;
    std::function<void(std::size_t, std::size_t, double)> visit =
        [&](std::size_t next, std::size_t left, double weight) {
            if (left == 0) {
                best = std::min(best, weight);
                return;
            }
            if (edges.size() - next < left) return;
            for (std::size_t i = next; i + left <= edges.size(); ++i) {
                sg::DisjointSet candidate(dsu_nodes);
                bool forest = true;
                for (const std::size_t selected : choice) {
                    const auto& edge = edges[selected];
                    if (!candidate.unite(edge.a, edge.b)) {
                        forest = false;
                        break;
                    }
                }
                if (!forest || !candidate.unite(edges[i].a, edges[i].b)) continue;
                choice.push_back(i);
                visit(i + 1, left - 1, weight + edges[i].weight);
                choice.pop_back();
            }
        };
    visit(0, need, 0.0);
    return best;
}

void test_kruskal() {
    Suite suite("Kruskal");
    Rng rng(3);
    constexpr double big = 1e6;
    for (int case_index = 0; case_index < 120; ++case_index) {
        suite.begin_case();
        const int n = random_int(rng, 2, 6);
        std::vector<std::string> nodes;
        for (int i = 0; i < n; ++i) nodes.push_back("n" + std::to_string(i));
        const int line_count = random_int(rng, 1, 7);
        std::vector<sg::Line> lines;
        for (int i = 0; i < line_count; ++i) {
            const int ai = random_int(rng, 0, n - 1);
            int bi = random_int(rng, 0, n - 1);
            while (bi == ai) bi = random_int(rng, 0, n - 1);
            lines.push_back({"L" + std::to_string(i), nodes[static_cast<std::size_t>(ai)],
                             nodes[static_cast<std::size_t>(bi)],
                             std::round(random_real(rng, 0.1, 3.0) * 1000.0) / 1000.0, 10.0, false});
        }
        const int primary_count = random_int(rng, 1, std::min(2, n));
        const auto primary = sample_without_replacement(rng, nodes, static_cast<std::size_t>(primary_count));
        std::vector<std::string> remaining;
        for (const auto& node : nodes) {
            if (std::find(primary.begin(), primary.end(), node) == primary.end()) remaining.push_back(node);
        }
        std::vector<std::string> backup;
        if (!remaining.empty() && random_int(rng, 0, 1) != 0) backup.push_back(remaining.front());

        const sg::RadialForest forest = sg::kruskal_radial(nodes, lines, primary, backup);
        std::vector<WeightedEdge> candidates;
        for (const auto& source : primary) candidates.push_back({"__ROOT__", source, 0.0});
        for (const auto& line : lines) candidates.push_back({line.a, line.b, line.resistance});
        for (const auto& source : backup) candidates.push_back({"__ROOT__", source, big});
        const double expected = brute_force_forest_weight(nodes, candidates);
        const double actual = forest.total_weight + big * static_cast<double>(forest.island_roots.size());
        suite.require(std::isfinite(expected) && std::abs(actual - expected) <= 1e-9,
                      "radial forest weight differs from brute-force forest");

        sg::DisjointSet dsu(nodes);
        std::map<std::string, sg::Line> by_id;
        for (const auto& line : lines) by_id[line.id] = line;
        for (const auto& id : forest.lines) {
            suite.require(dsu.unite(by_id.at(id).a, by_id.at(id).b), "accepted physical lines contain a loop");
        }
        std::map<std::string, std::vector<std::string>> roots_by_component;
        for (const auto& root : forest.roots) roots_by_component[dsu.find(root)].push_back(root);
        bool one_root = true;
        for (const auto& item : roots_by_component) one_root = one_root && item.second.size() == 1;
        suite.require(one_root, "each physical tree does not have exactly one root");
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("Kruskal suite failed");
}

struct Arc {
    int from = 0;
    int to = 0;
    int capacity = 0;
};

int brute_min_cut(int node_count, const std::vector<Arc>& arcs, int source, int sink) {
    std::vector<int> others;
    for (int node = 0; node < node_count; ++node) {
        if (node != source && node != sink) others.push_back(node);
    }
    const int intermediate = static_cast<int>(others.size());
    int best = std::numeric_limits<int>::max();
    for (int mask = 0; mask < (1 << intermediate); ++mask) {
        std::set<int> side{source};
        for (int bit = 0; bit < intermediate; ++bit) {
            if ((mask & (1 << bit)) != 0) side.insert(others[static_cast<std::size_t>(bit)]);
        }
        int capacity = 0;
        for (const auto& arc : arcs) {
            if (side.count(arc.from) != 0 && side.count(arc.to) == 0) capacity += arc.capacity;
        }
        best = std::min(best, capacity);
    }
    return best;
}

std::string flow_label(int node) { return "v" + std::to_string(node); }

void check_flow_case(Suite& suite, Rng& rng, int case_index) {
    suite.begin_case();
    const int n = random_int(rng, 2, 7);
    const std::string source = flow_label(0);
    const std::string sink = flow_label(n - 1);
    sg::FlowNetwork network;
    for (int node = 0; node < n; ++node) network.node(flow_label(node));
    std::vector<Arc> arcs;
    const int edge_count = random_int(rng, 0, 14);
    for (int edge_index = 0; edge_index < edge_count; ++edge_index) {
        const int from = random_int(rng, 0, n - 1);
        int to = random_int(rng, 0, n - 1);
        while (to == from) to = random_int(rng, 0, n - 1);
        const int capacity = random_int(rng, 0, 20);
        if (random_int(rng, 0, 9) < 4) {
            network.add_edge(flow_label(from), flow_label(to), capacity, capacity);
            arcs.push_back({from, to, capacity});
            arcs.push_back({to, from, capacity});
        } else {
            network.add_edge(flow_label(from), flow_label(to), capacity);
            arcs.push_back({from, to, capacity});
        }
    }
    const long long value = network.max_flow(source, sink);
    suite.require(value == brute_min_cut(n, arcs, 0, n - 1), "max-flow value differs from enumerated min-cut");
    suite.require(value == network.value(source), "source flow value differs from max-flow return");
    for (std::size_t edge = 0; edge < network.head.size(); ++edge) {
        suite.require(network.flow[edge] <= network.cap[edge], "flow exceeds an arc capacity");
        suite.require(network.flow[edge] == -network.flow[edge ^ 1], "residual twin flows are not antisymmetric");
    }
    for (int node = 1; node < n - 1; ++node) {
        long long conservation = 0;
        for (const int edge : network.adj[static_cast<std::size_t>(network.index.at(flow_label(node)))]) {
            conservation += network.flow[static_cast<std::size_t>(edge)];
        }
        suite.require(conservation == 0, "internal flow conservation failed");
    }
    const std::set<std::string> side = network.min_cut_source_side(source);
    suite.require(side.count(sink) == 0, "minimum-cut source side contains sink");
    long long cut_capacity = 0;
    for (const int edge : network.cut_arcs(side)) cut_capacity += network.cap[static_cast<std::size_t>(edge)];
    suite.require(cut_capacity == value, "residual reachable cut capacity differs from flow value");
    (void)case_index;
    suite.end_case();
}

void test_max_flow() {
    Suite suite("Edmonds-Karp");
    Rng rng(4);
    for (int case_index = 0; case_index < 250; ++case_index) check_flow_case(suite, rng, case_index);

    Rng phased_rng(5);
    for (int case_index = 0; case_index < 100; ++case_index) {
        suite.begin_case();
        const int n = random_int(phased_rng, 3, 7);
        const int edge_count = random_int(phased_rng, 2, 12);
        std::vector<Arc> spec;
        for (int edge = 0; edge < edge_count; ++edge) {
            const int from = random_int(phased_rng, 0, n - 1);
            int to = random_int(phased_rng, 0, n - 1);
            while (to == from) to = random_int(phased_rng, 0, n - 1);
            spec.push_back({from, to, random_int(phased_rng, 1, 15)});
        }
        sg::FlowNetwork phased;
        sg::FlowNetwork fresh;
        std::vector<int> arc_ids;
        for (const auto& edge : spec) {
            arc_ids.push_back(phased.add_edge(flow_label(edge.from), flow_label(edge.to), 0));
            fresh.add_edge(flow_label(edge.from), flow_label(edge.to), edge.capacity);
        }
        for (std::size_t i = 0; i < spec.size(); ++i) {
            phased.set_capacity(arc_ids[i], spec[i].capacity);
            phased.max_flow(flow_label(0), flow_label(n - 1));
        }
        const long long expected = fresh.max_flow(flow_label(0), flow_label(n - 1));
        suite.require(phased.value(flow_label(0)) == expected, "phased capacity augmentation differs from fresh run");
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("Edmonds-Karp suite failed");
}

void test_shortest_paths() {
    Suite suite("Shortest paths");
    Rng rng(6);
    for (int case_index = 0; case_index < 150; ++case_index) {
        suite.begin_case();
        const RandomGraph graph = random_graph(rng, random_int(rng, 2, 10), random_int(rng, 0, 18));
        const int source_count = random_int(rng, 1, std::min(3, static_cast<int>(graph.nodes.size())));
        const auto sources = sample_without_replacement(rng, graph.nodes, static_cast<std::size_t>(source_count));
        std::map<std::string, double> weights;
        for (const auto& edge : graph.edges) weights[std::get<3>(edge)] = std::get<2>(edge);
        const double inf = std::numeric_limits<double>::infinity();
        std::map<std::string, double> bellman;
        for (const auto& node : graph.nodes) bellman[node] = inf;
        for (const auto& source : sources) bellman[source] = 0.0;
        for (std::size_t pass = 0; pass < graph.nodes.size(); ++pass) {
            for (const auto& edge : graph.edges) {
                const auto& a = std::get<0>(edge);
                const auto& b = std::get<1>(edge);
                const double weight = std::get<2>(edge);
                bellman[b] = std::min(bellman[b], bellman[a] + weight);
                bellman[a] = std::min(bellman[a], bellman[b] + weight);
            }
        }

        const auto weight = [&](const std::string& id) { return weights.at(id); };
        const sg::ShortestPaths dijkstra = sg::dijkstra(graph.adjacency, sources, weight);
        const sg::ShortestPaths floyd_multi = sg::shortest_paths_from_sources(
            graph.adjacency, graph.nodes, sources, weight, "floyd-warshall");
        const std::vector<std::tuple<std::string, std::string, double, std::string>> all_edges = graph.edges;
        const sg::AllPairs all_pairs = sg::floyd_warshall(graph.nodes, all_edges);
        for (const auto& node : graph.nodes) {
            const double expected = bellman.at(node);
            const double dj = dijkstra.dist.count(node) ? dijkstra.dist.at(node) : inf;
            const double fw = floyd_multi.dist.count(node) ? floyd_multi.dist.at(node) : inf;
            double pairwise = inf;
            for (const auto& source : sources) pairwise = std::min(pairwise, all_pairs.distance(source, node));
            const bool expected_reachable = std::isfinite(expected);
            suite.require(std::isfinite(dj) == expected_reachable &&
                              (!expected_reachable || std::abs(dj - expected) <= 1e-9),
                          "Dijkstra distance differs for " + node);
            suite.require(std::isfinite(fw) == expected_reachable &&
                              (!expected_reachable || std::abs(fw - expected) <= 1e-9),
                          "Floyd multi-source distance differs for " + node);
            suite.require(std::isfinite(pairwise) == expected_reachable &&
                              (!expected_reachable || std::abs(pairwise - expected) <= 1e-9),
                          "all-pairs distance differs for " + node);
            for (const sg::ShortestPaths* paths : {&dijkstra, &floyd_multi}) {
                if (!paths->dist.count(node)) continue;
                suite.require(std::find(sources.begin(), sources.end(), paths->root.at(node)) != sources.end(),
                              "reconstructed root is not a requested source for " + node);
                double path_weight = 0.0;
                for (const auto& line : paths->lines_to(node)) path_weight += weights.at(line);
                suite.require(std::abs(path_weight - paths->dist.at(node)) <= 1e-9,
                              "reconstructed path length differs for " + node);
            }
        }
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("Shortest-path suite failed");
}

double brute_battery_cost(const std::vector<double>& net,
                          const std::vector<double>& price,
                          const std::vector<double>& export_price,
                          const sg::BatteryModel& model,
                          const std::vector<double>& reserve,
                          int start_level,
                          double step,
                          double degradation) {
    const int horizon = static_cast<int>(net.size());
    const int levels = static_cast<int>(std::round(model.energy_mwh / step));
    const int rate_up = static_cast<int>(std::floor(model.charge_mw * model.efficiency_charge / step + 1e-9));
    const int rate_down = static_cast<int>(std::floor(model.discharge_mw / model.efficiency_discharge / step + 1e-9));
    int combinations = 1;
    for (int t = 0; t < horizon; ++t) combinations *= levels + 1;
    double best = std::numeric_limits<double>::infinity();
    std::vector<int> state(static_cast<std::size_t>(horizon + 1), 0);
    state[0] = start_level;
    for (int code = 0; code < combinations; ++code) {
        int value = code;
        for (int t = 1; t <= horizon; ++t) {
            state[static_cast<std::size_t>(t)] = value % (levels + 1);
            value /= levels + 1;
        }
        if (state.back() < start_level) continue;
        bool valid = true;
        for (int t = 0; t <= horizon; ++t) {
            if (state[static_cast<std::size_t>(t)] * step < reserve[static_cast<std::size_t>(t)] - 1e-9) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        double cost = 0.0;
        for (int t = 0; t < horizon; ++t) {
            const int delta_level = state[static_cast<std::size_t>(t + 1)] - state[static_cast<std::size_t>(t)];
            if (delta_level < -rate_down || delta_level > rate_up) {
                valid = false;
                break;
            }
            const double delta = delta_level * step;
            cost += sg::hourly_cost(net[static_cast<std::size_t>(t)] + sg::grid_power(delta, model),
                                    price[static_cast<std::size_t>(t)], export_price[static_cast<std::size_t>(t)]) +
                     degradation * std::abs(delta);
        }
        if (valid) best = std::min(best, cost);
    }
    return best;
}

void test_battery_dp() {
    Suite suite("Battery DP");
    Rng rng(7);
    for (int case_index = 0; case_index < 40; ++case_index) {
        suite.begin_case();
        constexpr int horizon = 5;
        constexpr double step = 0.5;
        const sg::BatteryModel model{
            2.0,
            static_cast<double>(random_int(rng, 1, 3)) * 0.5,
            static_cast<double>(random_int(rng, 1, 3)) * 0.5,
            random_int(rng, 0, 1) == 0 ? 0.9 : 1.0,
            random_int(rng, 0, 1) == 0 ? 0.9 : 1.0,
        };
        std::vector<double> net, price, export_price(horizon, 10.0), reserve(horizon + 1);
        for (int t = 0; t < horizon; ++t) {
            net.push_back(random_real(rng, -2.0, 4.0));
            price.push_back(random_real(rng, 20.0, 150.0));
        }
        for (double& item : reserve) item = random_int(rng, 0, 1) == 0 ? 0.0 : 0.5;
        reserve[0] = std::min(reserve[0], 1.0);
        const double expected = brute_battery_cost(net, price, export_price, model, reserve, 2, step, 1.0);
        bool threw = false;
        sg::BatterySchedule schedule;
        try {
            schedule = sg::dp_schedule(net, price, export_price, model, reserve, 1.0,
                                       std::nullopt, step, 1.0);
        } catch (const std::exception&) {
            threw = true;
        }
        if (!std::isfinite(expected)) {
            suite.require(threw, "DP accepted an infeasible reserve/rate instance");
        } else {
            suite.require(!threw, "DP rejected a feasible reserve/rate instance");
            if (!threw) {
                suite.require(std::abs(schedule.cost - expected) <= 1e-6,
                              "DP cost differs from brute-force optimum");
                suite.require(schedule.soc_mwh.size() == static_cast<std::size_t>(horizon + 1),
                              "DP SOC sequence has the wrong horizon");
                for (int t = 0; t <= horizon && static_cast<std::size_t>(t) < schedule.soc_mwh.size(); ++t) {
                    suite.require(schedule.soc_mwh[static_cast<std::size_t>(t)] + 1e-9 >= reserve[static_cast<std::size_t>(t)],
                                  "DP schedule violates reserve at hour " + std::to_string(t));
                }
                for (int t = 0; t < horizon && static_cast<std::size_t>(t) < schedule.power_mw.size(); ++t) {
                    suite.require(schedule.power_mw[static_cast<std::size_t>(t)] <= model.charge_mw + 1e-9,
                                  "DP schedule exceeds charge rate");
                    suite.require(schedule.power_mw[static_cast<std::size_t>(t)] >= -model.discharge_mw - 1e-9,
                                  "DP schedule exceeds discharge rate");
                }
                suite.require(schedule.soc_mwh.back() + 1e-9 >= schedule.soc_mwh.front(),
                              "DP schedule misses terminal SOC target");
            }
        }
        suite.end_case();
    }
    std::cout << suite.name << ' ' << (suite.cases - suite.failed_cases) << '/' << suite.cases
              << " cases, " << suite.checks << " checks\n";
    if (suite.failures != 0) throw std::runtime_error("Battery-DP suite failed");
}

} // namespace

int main() {
    try {
        test_bfs();
        test_disjoint_set();
        test_kruskal();
        test_max_flow();
        test_shortest_paths();
        test_battery_dp();
        std::cout << "ALL C++ ALGORITHM ORACLE TESTS PASSED\n";
        return 0;
    } catch (const std::exception& exc) {
        std::cerr << "test_cpp_algorithms: " << exc.what() << '\n';
        return 1;
    }
}

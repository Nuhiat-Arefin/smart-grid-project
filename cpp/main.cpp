#include "smartgrid.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void usage(std::ostream& out) {
    out << "Smart Grid native C++17 adapter\n\n"
        << "Usage:\n"
        << "  smartgrid serve [--port N] [--web-root PATH]\n"
        << "  smartgrid demo\n"
        << "  smartgrid event [--hour N] [--faults L01,L02] [--policy POLICY] [--battery POLICY]\n"
        << "  smartgrid boot\n"
        << "  smartgrid battery\n"
        << "  smartgrid evaluate [--events N] [--seed N] [--output PATH]\n\n"
        << "Policies: proposal, mincut, engine, global-exchange.\n"
        << "Battery policies are read from the boot payload (normally dp, dp_no_reserve, threshold, idle).\n";
}

int parse_int(const std::string& text, const std::string& name, int min, int max) {
    std::size_t consumed = 0;
    try {
        const long value = std::stol(text, &consumed);
        if (consumed != text.size() || value < min || value > max) throw std::runtime_error("");
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw std::runtime_error(name + " must be an integer from " + std::to_string(min) + " to " + std::to_string(max));
    }
}

unsigned parse_seed(const std::string& text) {
    std::size_t consumed = 0;
    try {
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size() || value > std::numeric_limits<unsigned>::max()) throw std::runtime_error("");
        return static_cast<unsigned>(value);
    } catch (const std::exception&) {
        throw std::runtime_error("seed must be a non-negative unsigned integer");
    }
}

std::vector<std::string> split_faults(const std::string& text) {
    std::vector<std::string> faults;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos
                                                      ? std::string::npos : comma - start);
        if (!item.empty()) faults.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return faults;
}

void require_value(int argc, int& index, const std::string& option) {
    if (index + 1 >= argc) throw std::runtime_error(option + " requires a value");
    ++index;
}

int command_serve(int argc, char** argv) {
    int port = 8000;
    std::string web_root = "web";
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--port") {
            require_value(argc, i, option);
            port = parse_int(argv[i], "port", 1, 65535);
        } else if (option == "--web-root") {
            require_value(argc, i, option);
            web_root = argv[i];
        } else if (option == "--help" || option == "-h") {
            usage(std::cout);
            return 0;
        } else {
            throw std::runtime_error("unknown serve option: " + option);
        }
    }
    return sg::run_server(port, web_root);
}

int command_event(int argc, char** argv) {
    int hour = 19;
    std::vector<std::string> faults;
    std::string policy = "engine";
    std::string battery = "dp";
    bool audit = true;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--hour") {
            require_value(argc, i, option);
            hour = parse_int(argv[i], "hour", 0, 23);
        } else if (option == "--faults") {
            require_value(argc, i, option);
            faults = split_faults(argv[i]);
        } else if (option == "--policy") {
            require_value(argc, i, option);
            policy = argv[i];
        } else if (option == "--battery" || option == "--battery-policy") {
            require_value(argc, i, option);
            battery = argv[i];
        } else if (option == "--no-audit") {
            audit = false;
        } else if (option == "--help" || option == "-h") {
            usage(std::cout);
            return 0;
        } else {
            throw std::runtime_error("unknown event option: " + option);
        }
    }
    std::cout << sg::run_event(hour, faults, policy, battery, audit).dump(2) << '\n';
    return 0;
}

int command_demo() {
    const sg::Json payload = sg::boot();
    const sg::Json presets = payload["presets"];
    if (!presets.is_array() || presets.elements().empty()) {
        throw std::runtime_error("boot payload has no demo presets");
    }
    for (const sg::Json& preset : presets.elements()) {
        const std::string name = preset["name"].string(preset["id"].string("scenario"));
        const int hour = static_cast<int>(preset["hour"].number(19));
        const sg::Json faults_json = preset["faults"];
        std::vector<std::string> faults;
        if (faults_json.is_array()) {
            for (const sg::Json& fault : faults_json.elements()) faults.push_back(fault.string());
        }
        const sg::Json result = sg::run_event(hour, faults, "engine", "dp", true);
        const sg::Json summary = result["summary"];
        std::cout << "\n" << name << "\n"
                  << "  hour " << hour << ":00; faults: ";
        if (faults.empty()) std::cout << "none";
        else {
            for (std::size_t i = 0; i < faults.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << faults[i];
            }
        }
        std::cout << "\n  served " << summary["total_served_mw"].number()
                  << " / " << summary["total_demand_mw"].number() << " MW"
                  << "; switching operations " << summary["switch_ops"].integer()
                  << "; exchanges " << summary["exchanges"].integer() << '\n';
    }
    return 0;
}

int command_evaluate(int argc, char** argv) {
    int count = 2000;
    unsigned seed = 2026;
    std::filesystem::path output = std::filesystem::path("results") / "evaluation.json";
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--events") {
            require_value(argc, i, option);
            count = parse_int(argv[i], "events", 1, 1000000);
        } else if (option == "--seed") {
            require_value(argc, i, option);
            seed = parse_seed(argv[i]);
        } else if (option == "--output" || option == "--out") {
            require_value(argc, i, option);
            output = argv[i];
        } else if (option == "--help" || option == "-h") {
            usage(std::cout);
            return 0;
        } else {
            throw std::runtime_error("unknown evaluate option: " + option);
        }
    }
    const sg::Json result = sg::evaluate(count, seed);
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open evaluation output: " + output.string());
    file << result.dump(1) << '\n';
    if (!file) throw std::runtime_error("cannot write evaluation output: " + output.string());
    std::cout << "wrote " << output.string() << '\n';
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        usage(std::cout);
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (command == "serve") return command_serve(argc, argv);
    if (command == "demo") {
        if (argc > 2 && (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
            usage(std::cout);
            return 0;
        }
        if (argc > 2) throw std::runtime_error("demo takes no options");
        return command_demo();
    }
    if (command == "event") return command_event(argc, argv);
    if (command == "boot") {
        if (argc > 2) throw std::runtime_error("boot takes no options");
        std::cout << sg::boot().dump(2) << '\n';
        return 0;
    }
    if (command == "battery") {
        if (argc > 2) throw std::runtime_error("battery takes no options");
        std::cout << sg::battery_plans().dump(2) << '\n';
        return 0;
    }
    if (command == "evaluate") return command_evaluate(argc, argv);
    throw std::runtime_error("unknown command: " + command);
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exc) {
        std::cerr << "smartgrid: " << exc.what() << "\n";
        return 2;
    }
}

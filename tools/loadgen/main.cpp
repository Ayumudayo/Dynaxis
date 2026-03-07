#include "report_writer.hpp"
#include "scenario_loader.hpp"
#include "scenario_runner.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        const auto cli = loadgen::parse_cli(argc, argv);
        const auto scenario = loadgen::load_scenario(cli.scenario_path);
        const auto summary = loadgen::run_scenario(scenario, cli);
        const auto report = loadgen::to_json(summary);
        loadgen::write_report(cli.report_path, report);
        loadgen::print_summary(summary, cli.report_path);
        return summary.error_count == 0 && summary.connected_sessions > 0 ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "loadgen error: " << ex.what() << '\n';
        return 1;
    }
}

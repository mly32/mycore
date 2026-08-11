#include "dots/app_cli/app_cli.hpp"
#include "dots/server/input_provenance.hpp"
#include "dots/server/server_runtime.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kHelp = R"(Dots Server
Runs the authoritative Dots simulation at 30 Hz.

Usage:
  dots_server [--listen <address>] [--ticks <count>]
              [--respawn-cooldown-ticks <count>]
              [--input-provenance-trace <path>]
              [--input-provenance-max-records <count>]
              [--fake-lag-ms <milliseconds>] [--fake-loss-percent <percent>] [--help]

Options:
  --listen <address>           Listen on a numeric IPv4 or bracketed IPv6 address.
                               Defaults to [::]:27020. Port 0 selects a private dynamic port.
  --ticks <count>              Run exactly this many simulation ticks, then exit.
  --respawn-cooldown-ticks <n> Allow respawn this many server ticks after defeat (default 90).
  --input-provenance-trace <p> Write schema-versioned applied-input JSONL to a new file.
  --input-provenance-max-records <n>
                               Maximum applied-input rows written to the trace. Required with
                               --input-provenance-trace; aggregate statistics continue afterward.
  --fake-lag-ms <milliseconds> Add outgoing one-way packet delay in native mode.
  --fake-loss-percent <value>  Drop this percentage of outgoing packets (0..100).
  --help                       Show this help text and exit.

The server is authoritative and headless. It prints DOTS_SERVER_READY after binding.
)";

volatile std::sig_atomic_t stop_requested{};

void request_stop(int) {
    stop_requested = 1;
}

struct Options {
    std::optional<std::uint64_t> ticks;
    std::string listen_address{"[::]:27020"};
    std::uint32_t respawn_cooldown_ticks{dots::server::kDefaultRespawnCooldownTicks};
    std::optional<std::filesystem::path> input_provenance_trace;
    std::optional<std::uint64_t> input_provenance_max_records;
    mycore::net_transport::NetworkImpairment impairment;
    bool help{};
};

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--listen") {
            options.listen_address =
                dots::app_cli::require_option_value(index, argc, argv, argument);
            continue;
        }
        if (dots::app_cli::consume_network_impairment_option(
                argument, index, argc, argv, options.impairment)) {
            continue;
        }
        if (argument == "--respawn-cooldown-ticks") {
            options.respawn_cooldown_ticks = dots::app_cli::parse_nonnegative_u32(
                dots::app_cli::require_option_value(index, argc, argv, argument), argument);
            continue;
        }
        if (argument == "--input-provenance-trace" && !options.input_provenance_trace) {
            options.input_provenance_trace =
                dots::app_cli::require_option_value(index, argc, argv, argument);
            continue;
        }
        if (argument == "--input-provenance-max-records" && !options.input_provenance_max_records) {
            options.input_provenance_max_records = dots::app_cli::parse_positive_u64(
                dots::app_cli::require_option_value(index, argc, argv, argument), argument);
            continue;
        }
        if (argument != "--ticks" || options.ticks) {
            throw std::runtime_error{"invalid argument: " + std::string{argument}};
        }
        options.ticks = dots::app_cli::parse_positive_u64(
            dots::app_cli::require_option_value(index, argc, argv, argument), argument);
    }
    if (options.input_provenance_trace.has_value() !=
        options.input_provenance_max_records.has_value()) {
        throw std::runtime_error{
            "--input-provenance-trace and --input-provenance-max-records must be used together"};
    }
    return options;
}

class InputProvenanceArtifact {
public:
    InputProvenanceArtifact(const std::filesystem::path& path, std::uint64_t maximum_records)
        : maximum_records_(maximum_records) {
        auto error = std::error_code{};
        if (std::filesystem::exists(path, error)) {
            throw std::runtime_error{"input provenance trace already exists: " + path.string()};
        }
        if (error) {
            throw std::runtime_error{"could not inspect input provenance trace path: " +
                                     error.message()};
        }
        const auto parent = path.parent_path();
        if (!parent.empty() && !std::filesystem::is_directory(parent, error)) {
            throw std::runtime_error{"input provenance trace parent is not a directory: " +
                                     parent.string()};
        }
        if (error) {
            throw std::runtime_error{"could not inspect input provenance trace parent: " +
                                     error.message()};
        }
        stream_.open(path, std::ios::out | std::ios::app);
        if (!stream_) {
            throw std::runtime_error{"could not create input provenance trace: " + path.string()};
        }
        if (stream_.tellp() != std::streampos{}) {
            throw std::runtime_error{"input provenance trace already exists: " + path.string()};
        }
        write_line(dots::server::input_provenance_header_jsonl(maximum_records_));
    }

    void append(const std::vector<dots::server::InputProvenanceRecord>& records) {
        for (const auto& record : records) {
            if (records_written_ < maximum_records_) {
                write_line(dots::server::input_provenance_record_jsonl(record));
                ++records_written_;
            } else {
                ++records_omitted_;
                if (!limit_warning_logged_) {
                    mycore::debug::log_warning(
                        "dots.server.input",
                        "Input provenance trace reached its {}-record limit; continuing exact "
                        "aggregate statistics",
                        maximum_records_);
                    limit_warning_logged_ = true;
                }
            }
        }
    }

    void finish(const dots::server::InputProvenanceSummary& runtime,
                std::uint64_t final_server_tick) {
        write_line(dots::server::input_provenance_summary_jsonl({
            .runtime = runtime,
            .records_written = records_written_,
            .records_omitted_by_limit = records_omitted_,
            .final_server_tick = final_server_tick,
            .complete = true,
        }));
        stream_.flush();
        if (!stream_) {
            throw std::runtime_error{"could not finalize input provenance trace"};
        }
    }

    [[nodiscard]] std::uint64_t records_written() const noexcept {
        return records_written_;
    }

    [[nodiscard]] std::uint64_t records_omitted() const noexcept {
        return records_omitted_;
    }

private:
    void write_line(const std::string& line) {
        stream_ << line << '\n';
        if (!stream_) {
            throw std::runtime_error{"could not write input provenance trace"};
        }
    }

    std::ofstream stream_;
    std::uint64_t maximum_records_{};
    std::uint64_t records_written_{};
    std::uint64_t records_omitted_{};
    bool limit_warning_logged_{};
};

void log_input_provenance_summary(const dots::server::InputProvenanceSummary& summary,
                                  std::uint64_t records_written,
                                  std::uint64_t records_omitted) {
    const auto wait = dots::server::input_queue_wait_statistics(summary);
    if (wait.count == 0) {
        mycore::debug::log_info(
            "dots.server.input",
            "Input provenance: accepted {}, applied 0, pending {}, discarded {}, trace written {}, "
            "omitted {}, "
            "buffer dropped {}",
            summary.accepted_input_count,
            summary.pending_input_count,
            summary.discarded_before_application_count,
            records_written,
            records_omitted,
            summary.record_buffer_dropped_count);
        return;
    }
    mycore::debug::log_info(
        "dots.server.input",
        "Input provenance: accepted {}, applied {}, pending {}, discarded {}, trace written {}, "
        "omitted {}, "
        "buffer dropped {}, wait ticks mean {:.2f} p50 {} p95 {} p99 {} max {}",
        summary.accepted_input_count,
        summary.applied_input_count,
        summary.pending_input_count,
        summary.discarded_before_application_count,
        records_written,
        records_omitted,
        summary.record_buffer_dropped_count,
        wait.mean_ticks,
        wait.percentile_50_ticks.value_or(0),
        wait.percentile_95_ticks.value_or(0),
        wait.percentile_99_ticks.value_or(0),
        wait.maximum_ticks.value_or(0));
}

} // namespace

int main(int argc, char** argv) {
    const mycore::debug::Runtime logging;
    try {
        const auto options = parse_arguments(argc, argv);
        if (options.help) {
            std::cout << kHelp;
            return 0;
        }

        dots::simulation::World world;
        if (!dots::simulation::spawn_default_food_field(world)) {
            throw std::runtime_error{"Could not populate the authoritative Dots world"};
        }
        const auto listen_address =
            dots::app_cli::parse_listen_address(options.listen_address, "--listen");
        mycore::net_transport::GameNetworkingSocketsNetwork network{options.impairment};
        const auto listening = network.listen(listen_address);
        auto input_provenance_artifact = std::optional<InputProvenanceArtifact>{};
        if (options.input_provenance_trace) {
            input_provenance_artifact.emplace(*options.input_provenance_trace,
                                              options.input_provenance_max_records.value_or(0));
        }
        dots::server::Runtime server{
            *listening.endpoint,
            std::move(world),
            {
                .respawn_cooldown_ticks = options.respawn_cooldown_ticks,
                .input_provenance_record_capacity =
                    input_provenance_artifact ? dots::server::kDefaultInputProvenanceRecordCapacity
                                              : 0,
            },
        };

        std::cout << "DOTS_SERVER_READY " << listening.address.value() << '\n' << std::flush;

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        auto next_tick = std::chrono::steady_clock::now();
        const auto started_at = next_tick;
        auto last_overload_warning = next_tick - std::chrono::seconds{5};
        auto overload_active = false;
        auto healthy_tick_count = std::size_t{};
        auto catch_up_tick_count = std::size_t{};
        auto discarded_wall_time = std::chrono::steady_clock::duration::zero();
        std::uint64_t completed_ticks{};
        mycore::debug::log_info(
            "dots.server", "Authoritative 30 Hz server listening on {}", listening.address.value());
        while (stop_requested == 0 && (!options.ticks || completed_ticks < *options.ticks)) {
            const auto poll_started_at = std::chrono::steady_clock::now();
            const auto process_error = server.process_events();
            const auto step_started_at = std::chrono::steady_clock::now();
            const auto step_error = server.step();
            const auto work_completed_at = std::chrono::steady_clock::now();
            if (input_provenance_artifact) {
                input_provenance_artifact->append(server.take_input_provenance_records());
            }
            if (process_error || step_error) {
                throw std::runtime_error{
                    "The authoritative Dots server encountered a runtime error"};
            }
            ++completed_ticks;
            if (input_provenance_artifact && completed_ticks % 300U == 0U) {
                log_input_provenance_summary(server.input_provenance_summary(),
                                             input_provenance_artifact->records_written(),
                                             input_provenance_artifact->records_omitted());
            }
            next_tick += dots::simulation::kTickDuration;
            const auto late = work_completed_at >= next_tick;
            if (late) {
                ++catch_up_tick_count;
                healthy_tick_count = 0;
                const auto lag = work_completed_at - next_tick;
                if (!overload_active ||
                    work_completed_at - last_overload_warning >= std::chrono::seconds{5}) {
                    const auto poll_ms =
                        std::chrono::duration<double, std::milli>{step_started_at - poll_started_at}
                            .count();
                    const auto step_ms =
                        std::chrono::duration<double, std::milli>{work_completed_at -
                                                                  step_started_at}
                            .count();
                    const auto lag_ms = std::chrono::duration<double, std::milli>{lag}.count();
                    mycore::debug::log_warning(
                        "dots.server.simulation",
                        "Authoritative tick overload: poll {:.2f} ms, step {:.2f} ms, "
                        "deadline lag {:.2f} ms, catch-up tick {}/5",
                        poll_ms,
                        step_ms,
                        lag_ms,
                        catch_up_tick_count);
                    last_overload_warning = work_completed_at;
                }
                overload_active = true;
                if (catch_up_tick_count >= 5) {
                    discarded_wall_time += lag;
                    next_tick = work_completed_at + dots::simulation::kTickDuration;
                    catch_up_tick_count = 0;
                }
            } else {
                catch_up_tick_count = 0;
                if (overload_active && ++healthy_tick_count >= 30) {
                    mycore::debug::log_info(
                        "dots.server.simulation",
                        "Authoritative tick timing recovered after {} completed ticks",
                        completed_ticks);
                    overload_active = false;
                    healthy_tick_count = 0;
                }
            }
            std::this_thread::sleep_until(next_tick);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        const auto elapsed_seconds = std::chrono::duration<double>{elapsed}.count();
        const auto average_rate =
            elapsed_seconds > 0.0 ? static_cast<double>(completed_ticks) / elapsed_seconds : 0.0;
        if (input_provenance_artifact) {
            const auto summary = server.input_provenance_summary();
            input_provenance_artifact->finish(summary, server.world().tick().value());
            log_input_provenance_summary(summary,
                                         input_provenance_artifact->records_written(),
                                         input_provenance_artifact->records_omitted());
        }
        mycore::debug::log_info(
            "dots.server",
            "Stopped after {} ticks at {:.2f} average Hz with {:.2f} ms discarded wall-time debt",
            completed_ticks,
            average_rate,
            std::chrono::duration<double, std::milli>{discarded_wall_time}.count());
        return 0;
    } catch (const std::exception& error) {
        mycore::debug::log_error("dots.server", "{}", error.what());
        std::cerr << "dots_server: " << error.what() << '\n';
        return 1;
    }
}

// Event Sourcing Test - Manufacturing Facility Recovery Demo
//
// Demonstrates:
// 1. Parallel recovery of multiple equipment health actors
// 2. Zero-cost snapshot creation and loading
// 3. Event replay for state reconstruction
// 4. Recovery time benchmarking
//
// Test Scenario:
// - Lights-out manufacturing facility with N machines
// - Each machine has telemetry history (snapshots + events)
// - System restart: all actors recover in parallel
// - Metrics: recovery time, throughput, state consistency

#include "recovery_coordinator.hpp"
#include <caf/all.hpp>
#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>

using namespace caf;
using namespace recovery;
using namespace equipment_health;

// ============================================================================
// Mock Aeron Bridge - Simulates Kudu responses
// ============================================================================

behavior mock_aeron_bridge(event_based_actor* self) {
    return {
        [=](const std::string& tag, const std::string& request, actor response_handler) {
            if (tag != "kudu_request") return;

            // Parse request to determine operation
            std::string response;

            if (request.find("\"table\":\"equipment_snapshots\"") != std::string::npos) {
                // Snapshot query - return mock snapshot
                if (request.find("\"op\":\"scan\"") != std::string::npos) {
                    // Extract entity_id from request
                    size_t pos = request.find("\"entity_id\":\"");
                    std::string entity_id = "unknown";
                    if (pos != std::string::npos) {
                        pos += 14;
                        size_t end = request.find("\"", pos);
                        entity_id = request.substr(pos, end - pos);
                    }

                    // Mock snapshot data (seq 100, healthy state, some telemetry history)
                    response = "{\"status\":\"ok\""
                              ",\"rows\":[{"
                              "\"entity_id\":\"" + entity_id + "\""
                              ",\"sequence\":100"
                              ",\"timestamp\":1234567890"
                              ",\"snapshot_data\":\"{"
                              "\\\"equipment_id\\\":\\\"" + entity_id + "\\\""
                              ",\\\"state\\\":\\\"HEALTHY\\\""
                              ",\\\"sequence\\\":100"
                              ",\\\"max_vibration\\\":5.2"
                              ",\\\"max_temperature\\\":65.5"
                              ",\\\"avg_current\\\":12.3"
                              ",\\\"tool_wear\\\":0.5"
                              ",\\\"total_cycles\\\":1000"
                              ",\\\"telemetry_count\\\":100"
                              ",\\\"degradation_pct\\\":-1.0"
                              ",\\\"mtbf_hours\\\":-1.0"
                              ",\\\"maintenance_count\\\":2"
                              ",\\\"snapshot_seq\\\":100"
                              ",\\\"snapshot_ts\\\":1234567890"
                              "}\""
                              "}]}";
                } else if (request.find("\"op\":\"insert\"") != std::string::npos) {
                    // Snapshot insert - acknowledge
                    response = "{\"status\":\"ok\"}";
                }
            } else if (request.find("\"table\":\"equipment_events\"") != std::string::npos) {
                // Events query - return mock events
                if (request.find("\"op\":\"scan\"") != std::string::npos) {
                    // Extract entity_id
                    size_t pos = request.find("\"entity_id\":\"");
                    std::string entity_id = "unknown";
                    if (pos != std::string::npos) {
                        pos += 14;
                        size_t end = request.find("\"", pos);
                        entity_id = request.substr(pos, end - pos);
                    }

                    // Mock 10 events after snapshot (seq 101-110)
                    response = "{\"status\":\"ok\""
                              ",\"rows\":[";
                    for (int i = 1; i <= 10; i++) {
                        if (i > 1) response += ",";
                        response += "{"
                                   "\"entity_id\":\"" + entity_id + "\""
                                   ",\"sequence\":" + std::to_string(100 + i) +
                                   ",\"timestamp\":" + std::to_string(1234567890 + i * 100000) +
                                   ",\"event_type\":\"TelemetryReported\""
                                   ",\"event_data\":\"{}\""
                                   "}";
                    }
                    response += "]}";
                } else if (request.find("\"op\":\"insert\"") != std::string::npos) {
                    // Event insert - acknowledge
                    response = "{\"status\":\"ok\"}";
                }
            } else {
                response = "{\"status\":\"error\",\"message\":\"Unknown table\"}";
            }

            // Send response back to handler (simulate async delay)
            self->delayed_send(response_handler, std::chrono::milliseconds(1),
                              "kudu_response", response);
        }
    };
}

// ============================================================================
// Test Configuration
// ============================================================================

struct TestConfig {
    int32_t num_entities = 100;          // Number of machines to recover
    int32_t max_parallel_recoveries = 10; // Concurrent recovery limit
    bool verbose = false;                 // Print detailed logs

    void print() const {
        std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  EVENT SOURCING TEST - MANUFACTURING FACILITY RECOVERY       ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        std::cout << "Configuration:\n";
        std::cout << "  Entities (machines):    " << num_entities << "\n";
        std::cout << "  Max parallel recoveries: " << max_parallel_recoveries << "\n";
        std::cout << "  Verbose logging:        " << (verbose ? "yes" : "no") << "\n";
        std::cout << "══════════════════════════════════════════════════════════════\n\n";
    }
};

// ============================================================================
// Test Runner
// ============================================================================

void run_recovery_test(actor_system& system, const TestConfig& config) {
    config.print();

    // Create entity IDs (machine names)
    std::vector<std::string> entity_ids;
    for (int i = 1; i <= config.num_entities; i++) {
        std::ostringstream oss;
        oss << "machine-" << std::setfill('0') << std::setw(4) << i;
        entity_ids.push_back(oss.str());
    }

    std::cout << "Generated " << entity_ids.size() << " entity IDs\n";
    std::cout << "Sample entities: " << entity_ids[0] << ", " << entity_ids[1] << ", ...\n\n";

    // Spawn mock Aeron bridge
    auto bridge = system.spawn(mock_aeron_bridge);
    std::cout << "✓ Mock Aeron bridge spawned\n";

    // Spawn recovery coordinator
    auto coordinator = system.spawn(recovery_coordinator_actor, bridge, entity_ids);
    std::cout << "✓ Recovery coordinator spawned\n";

    // Configure parallel recovery limit
    anon_send(coordinator, "set_max_parallel", config.max_parallel_recoveries);
    std::cout << "✓ Set max parallel recoveries to " << config.max_parallel_recoveries << "\n\n";

    std::cout << "Starting parallel recovery...\n";
    std::cout << "──────────────────────────────────────────────────────────────\n\n";

    // Poll recovery status until complete
    auto start_time = std::chrono::steady_clock::now();
    bool recovery_complete = false;
    int poll_count = 0;

    // Create scoped_actor ONCE outside the loop (CAF best practice)
    scoped_actor self{system};

    while (!recovery_complete && poll_count < 100) {  // Max 10 seconds (100 * 100ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        poll_count++;

        // Request status (async)
        self->request(coordinator, std::chrono::seconds(1), "get_recovery_status").receive(
            [&](const std::string& status_json) {
                // Check if complete (match actual JSON format with spaces)
                if (status_json.find("\"status\": \"COMPLETE\"") != std::string::npos) {
                    recovery_complete = true;

                    auto end_time = std::chrono::steady_clock::now();
                    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        end_time - start_time).count();

                    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
                    std::cout << "║  RECOVERY TEST COMPLETE                                      ║\n";
                    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
                    std::cout << status_json << "\n\n";
                    std::cout << "Actual wall-clock time: " << duration_ms << "ms\n";
                    std::cout << "══════════════════════════════════════════════════════════════\n\n";
                } else if (config.verbose) {
                    // Parse and display progress
                    size_t pos = status_json.find("\"entities_recovered\":");
                    if (pos != std::string::npos) {
                        pos += 21;
                        int recovered = std::stoi(status_json.substr(pos));
                        std::cout << "Progress: " << recovered << "/" << config.num_entities
                                 << " entities recovered\n";
                    }
                }
            },
            [](const error& err) {
                std::cerr << "Error querying recovery status: " << to_string(err) << "\n";
            }
        );
    }

    if (!recovery_complete) {
        std::cerr << "\n✗ Recovery timed out after " << (poll_count * 100) << "ms\n";
        std::cerr << "  This may indicate a problem with the recovery coordinator.\n\n";
    } else {
        std::cout << "✓ Recovery test PASSED\n\n";
    }

    // Get final metrics (reuse the same scoped_actor)
    self->request(coordinator, std::chrono::seconds(1), "get_metrics").receive(
        [&](const std::string& metrics_json) {
            std::cout << "Final Metrics (JSON):\n";
            std::cout << metrics_json << "\n\n";
        },
        [](const error& err) {
            std::cerr << "Error getting final metrics: " << to_string(err) << "\n";
        }
    );

    // Cleanup
    anon_send_exit(coordinator, exit_reason::user_shutdown);
    anon_send_exit(bridge, exit_reason::user_shutdown);
}

// ============================================================================
// Main Entry Point
// ============================================================================

void caf_main(actor_system& system) {
    TestConfig config;

    // You can override defaults here or via command-line
    // For now, use reasonable defaults for testing
    config.num_entities = 100;
    config.max_parallel_recoveries = 10;
    config.verbose = false;

    run_recovery_test(system, config);
}

CAF_MAIN()

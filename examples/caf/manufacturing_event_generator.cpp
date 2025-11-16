// Manufacturing Event Generator - Lights-Out Factory Simulation
//
// Simulates a lights-out manufacturing facility with:
// - Multiple CNC machines, robotic cells, assembly stations
// - Realistic telemetry patterns (vibration, temperature, tool wear)
// - Predictive maintenance triggers (degradation detection)
// - Maintenance scheduling and completion workflows
// - High-frequency telemetry streams (configurable Hz)
//
// Use for:
// - Continuous profiling and stress testing
// - Recovery benchmarking with realistic data
// - Demo of event sourcing patterns
// - AI-Ops integration testing

#include "recovery_coordinator.hpp"
#include "equipment_health_actor.hpp"
#include <caf/all.hpp>
#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <random>
#include <cmath>

using namespace caf;
using namespace recovery;
using namespace equipment_health;
using namespace event_sourcing;

// ============================================================================
// Manufacturing Equipment Profiles
// ============================================================================

enum class EquipmentType {
    CNC_MILL,           // High-precision milling
    CNC_LATHE,          // Turning operations
    ROBOTIC_WELDER,     // Automated welding
    ASSEMBLY_STATION,   // Component assembly
    INSPECTION_CELL     // Quality inspection
};

struct EquipmentProfile {
    EquipmentType type;
    std::string name_prefix;

    // Normal operating ranges
    float vibration_mean;
    float vibration_stddev;
    float temperature_mean;
    float temperature_stddev;
    float current_mean;
    float current_stddev;

    // Tool wear characteristics
    float tool_wear_rate;  // mm per 1000 cycles
    float tool_replacement_threshold;

    // Degradation model
    float degradation_probability;  // Per 1000 cycles
    float critical_threshold_cycles;

    // Telemetry frequency
    float telemetry_hz;
};

const std::vector<EquipmentProfile> EQUIPMENT_PROFILES = {
    // CNC Mill - High vibration, moderate temperature
    {EquipmentType::CNC_MILL, "cnc-mill-",
     6.5f, 1.2f,   // Vibration: 6.5 ± 1.2 mm/s
     55.0f, 8.0f,  // Temperature: 55 ± 8°C
     15.0f, 2.5f,  // Current: 15 ± 2.5A
     0.08f, 2.0f,  // Tool wear: 0.08mm/1000 cycles, replace at 2mm
     0.005f, 100000,  // Degradation: 0.5% per 1000 cycles, critical at 100k
     10.0f},       // 10 Hz telemetry

    // CNC Lathe - Moderate vibration, high temperature
    {EquipmentType::CNC_LATHE, "cnc-lathe-",
     4.2f, 0.8f,   // Vibration: 4.2 ± 0.8 mm/s
     72.0f, 12.0f, // Temperature: 72 ± 12°C
     18.0f, 3.0f,  // Current: 18 ± 3A
     0.12f, 3.0f,  // Tool wear: 0.12mm/1000 cycles
     0.008f, 80000,
     10.0f},

    // Robotic Welder - Low vibration, very high temperature
    {EquipmentType::ROBOTIC_WELDER, "welder-",
     2.1f, 0.4f,   // Vibration: 2.1 ± 0.4 mm/s
     95.0f, 15.0f, // Temperature: 95 ± 15°C
     25.0f, 4.0f,  // Current: 25 ± 4A
     0.05f, 1.5f,  // Tool wear (electrode)
     0.003f, 120000,
     5.0f},        // 5 Hz telemetry (slower)

    // Assembly Station - Very low vibration, low temperature
    {EquipmentType::ASSEMBLY_STATION, "assembly-",
     0.8f, 0.2f,   // Vibration: 0.8 ± 0.2 mm/s
     35.0f, 5.0f,  // Temperature: 35 ± 5°C
     8.0f, 1.5f,   // Current: 8 ± 1.5A
     0.02f, 0.5f,  // Minimal tool wear
     0.001f, 200000,
     2.0f},        // 2 Hz telemetry

    // Inspection Cell - Minimal vibration, ambient temperature
    {EquipmentType::INSPECTION_CELL, "inspect-",
     0.3f, 0.1f,   // Vibration: 0.3 ± 0.1 mm/s
     28.0f, 3.0f,  // Temperature: 28 ± 3°C
     5.0f, 0.8f,   // Current: 5 ± 0.8A
     0.0f, 0.0f,   // No tool wear
     0.0005f, 500000,
     1.0f}         // 1 Hz telemetry
};

// ============================================================================
// Equipment Simulator - Per-Machine State
// ============================================================================

struct EquipmentSimulator {
    std::string entity_id;
    EquipmentProfile profile;
    actor health_actor;

    // Current state
    int64_t total_cycles = 0;
    float current_tool_wear = 0.0f;
    bool in_maintenance = false;
    int64_t maintenance_scheduled_at = 0;

    // Degradation state
    float degradation_pct = 0.0f;
    bool degradation_detected = false;

    // Timing
    std::chrono::steady_clock::time_point last_telemetry;
    std::chrono::milliseconds telemetry_interval;

    // Random number generation
    std::mt19937 rng;
    std::normal_distribution<float> vibration_dist;
    std::normal_distribution<float> temperature_dist;
    std::normal_distribution<float> current_dist;
    std::uniform_real_distribution<float> degradation_roll;

    EquipmentSimulator(const std::string& id, const EquipmentProfile& prof, actor actor_ref)
        : entity_id(id), profile(prof), health_actor(actor_ref),
          rng(std::random_device{}()),
          vibration_dist(prof.vibration_mean, prof.vibration_stddev),
          temperature_dist(prof.temperature_mean, prof.temperature_stddev),
          current_dist(prof.current_mean, prof.current_stddev),
          degradation_roll(0.0f, 1.0f) {

        telemetry_interval = std::chrono::milliseconds(
            static_cast<int>(1000.0f / prof.telemetry_hz));
        last_telemetry = std::chrono::steady_clock::now();
    }

    // Generate realistic telemetry sample
    void generateTelemetry(event_based_actor* self) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_telemetry < telemetry_interval) {
            return;  // Not time yet
        }
        last_telemetry = now;

        // Skip if in maintenance
        if (in_maintenance) return;

        // Generate telemetry values with realistic noise
        float vibration = std::max(0.0f, vibration_dist(rng));
        float temperature = std::max(0.0f, temperature_dist(rng));
        float current = std::max(0.0f, current_dist(rng));
        float spindle_speed = 1200.0f + (rng() % 400) - 200;  // 1000-1400 RPM

        // Add degradation effects
        if (degradation_pct > 0.0f) {
            vibration *= (1.0f + degradation_pct / 50.0f);  // Higher vibration
            temperature *= (1.0f + degradation_pct / 100.0f);  // Higher temp
        }

        // Update tool wear
        total_cycles++;
        current_tool_wear += profile.tool_wear_rate / 1000.0f;

        // Check for degradation detection (probabilistic)
        if (!degradation_detected && total_cycles % 1000 == 0) {
            if (degradation_roll(rng) < profile.degradation_probability) {
                degradation_pct = 30.0f + (rng() % 40);  // 30-70% degradation
                degradation_detected = true;

                // TODO: Send DegradationDetected event
                self->println("ALERT: {} degradation detected: {:.1f}%",
                             entity_id, degradation_pct);
            }
        }

        // Check if tool replacement needed
        if (current_tool_wear >= profile.tool_replacement_threshold && !in_maintenance) {
            scheduleMaintenance(self, "Tool replacement required");
        }

        // Check if critical degradation
        if (degradation_pct > 80.0f && !in_maintenance) {
            scheduleMaintenance(self, "Critical degradation - predictive maintenance");
        }

        // Send telemetry to health actor
        anon_mail("report_telemetry", vibration, temperature, current,
                 spindle_speed, current_tool_wear).send(health_actor);
    }

    void scheduleMaintenance(event_based_actor* self, const std::string& reason) {
        if (in_maintenance || maintenance_scheduled_at > 0) return;

        // Schedule maintenance for "soon" (simulated)
        maintenance_scheduled_at = currentTimeMicros() + (60 * 1000000);  // 1 minute from now

        anon_mail("schedule_maintenance", maintenance_scheduled_at,
                 "predictive", reason).send(health_actor);

        self->println("SCHEDULED: {} maintenance - {}", entity_id, reason);
    }

    void checkMaintenanceTrigger(event_based_actor* self) {
        if (maintenance_scheduled_at > 0 && !in_maintenance) {
            if (currentTimeMicros() >= maintenance_scheduled_at) {
                performMaintenance(self);
            }
        }
    }

    void performMaintenance(event_based_actor* self) {
        in_maintenance = true;

        std::string work_performed;
        if (current_tool_wear >= profile.tool_replacement_threshold) {
            work_performed = "Tool replacement + calibration";
            current_tool_wear = 0.0f;
        } else if (degradation_detected) {
            work_performed = "Component replacement + full inspection";
            degradation_pct = 0.0f;
            degradation_detected = false;
        } else {
            work_performed = "Preventive maintenance + lubrication";
        }

        int32_t duration = 15 + (rng() % 30);  // 15-45 minutes

        anon_mail("complete_maintenance", work_performed, duration).send(health_actor);

        self->println("MAINTENANCE: {} completed - {} ({}min)",
                     entity_id, work_performed, duration);

        // Reset state
        maintenance_scheduled_at = 0;
        in_maintenance = false;
    }
};

// ============================================================================
// Factory Simulator Actor
// ============================================================================

behavior factory_simulator(event_based_actor* self, actor aeron_bridge,
                          int num_machines_per_type) {

    // Create equipment simulators
    std::vector<EquipmentSimulator> simulators;

    for (const auto& profile : EQUIPMENT_PROFILES) {
        for (int i = 1; i <= num_machines_per_type; i++) {
            std::ostringstream oss;
            oss << profile.name_prefix << std::setfill('0') << std::setw(3) << i;
            std::string entity_id = oss.str();

            // Spawn equipment health actor
            auto health_actor = self->spawn(equipment_health_actor, entity_id, aeron_bridge);

            simulators.emplace_back(entity_id, profile, health_actor);
        }
    }

    self->println("\n╔══════════════════════════════════════════════════════════════╗");
    self->println("║  LIGHTS-OUT MANUFACTURING FACILITY SIMULATOR                 ║");
    self->println("╚══════════════════════════════════════════════════════════════╝");
    self->println("Total machines: {}", simulators.size());
    self->println("Equipment types: {}", EQUIPMENT_PROFILES.size());
    self->println("\nEquipment breakdown:");
    for (const auto& profile : EQUIPMENT_PROFILES) {
        self->println("  {} machines: {} ({}Hz telemetry)",
                     num_machines_per_type, profile.name_prefix, profile.telemetry_hz);
    }
    self->println("══════════════════════════════════════════════════════════════\n");
    self->println("Simulation running... Press Ctrl+C to stop.\n");

    // Main simulation loop - 10ms tick
    self->send(self, "tick");

    return {
        [=](const std::string& tag) mutable {
            if (tag != "tick") return;

            // Update all simulators
            for (auto& sim : simulators) {
                sim.generateTelemetry(self);
                sim.checkMaintenanceTrigger(self);
            }

            // Schedule next tick
            self->delayed_send(self, std::chrono::milliseconds(10), "tick");
        }
    };
}

// ============================================================================
// Main Entry Point
// ============================================================================

void caf_main(actor_system& system) {
    std::cout << "Starting manufacturing facility simulator...\n\n";

    // Configuration
    int num_machines_per_type = 5;  // 5 of each type = 25 total machines

    // Spawn mock Aeron bridge (replace with real Kudu service in production)
    auto bridge = system.spawn([](event_based_actor* self) -> behavior {
        return {
            [=](const std::string& tag, const std::string& request, actor response_handler) {
                if (tag != "kudu_request") return;
                // Acknowledge all requests (mock)
                self->delayed_send(response_handler, std::chrono::milliseconds(1),
                                  "kudu_response", "{\"status\":\"ok\"}");
            }
        };
    });

    std::cout << "✓ Aeron bridge spawned\n";

    // Spawn factory simulator
    auto factory = system.spawn(factory_simulator, bridge, num_machines_per_type);

    std::cout << "✓ Factory simulator spawned\n\n";

    // Run until Ctrl+C
    std::cout << "Simulation is running. Press Ctrl+C to stop.\n";
    std::cout << "Monitor with: watch -n 1 'ps aux | grep manufacturing'\n\n";

    // Wait for shutdown signal
    system.await_all_actors_done();
}

CAF_MAIN()

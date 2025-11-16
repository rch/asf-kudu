// Event Sourcing Foundations for CAF + Kudu
//
// Core abstractions for building event-sourced actor FSMs with:
// - Deterministic state rebuild from events
// - Parallel recovery from Kudu
// - Zero-cost snapshots (copy-on-write)
// - High-frequency telemetry support
//
// Design Philosophy:
// - Events are immutable facts (what happened)
// - Commands are requests (what should happen) - validated before events
// - State is derived from events (always rebuildable)
// - FSM transitions are deterministic
// - Snapshots are optimization, not source of truth

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <variant>
#include <optional>
#include <functional>

namespace event_sourcing {

// ============================================================================
// Core Event Types
// ============================================================================

// Base event - all events derive from this
struct Event {
    std::string entity_id;
    int64_t sequence;
    int64_t timestamp_us;
    std::string event_type;

    Event(const std::string& id, int64_t seq, int64_t ts, const std::string& type)
        : entity_id(id), sequence(seq), timestamp_us(ts), event_type(type) {}

    virtual ~Event() = default;
    virtual std::string toJSON() const = 0;
};

// ============================================================================
// Equipment Health Events (Industry 4.0)
// ============================================================================

// High-frequency telemetry (10Hz per machine)
struct TelemetryReported : public Event {
    float vibration_rms;      // mm/s
    float temperature_c;
    float motor_current_a;
    float spindle_speed_rpm;
    float tool_wear_mm;
    int32_t cycle_count;

    TelemetryReported(const std::string& id, int64_t seq, int64_t ts,
                     float vib, float temp, float curr, float speed, float wear, int32_t cycles)
        : Event(id, seq, ts, "TelemetryReported"),
          vibration_rms(vib), temperature_c(temp), motor_current_a(curr),
          spindle_speed_rpm(speed), tool_wear_mm(wear), cycle_count(cycles) {}

    std::string toJSON() const override;
};

// Equipment state change events
struct EquipmentStateChanged : public Event {
    std::string from_state;
    std::string to_state;
    std::string reason;

    EquipmentStateChanged(const std::string& id, int64_t seq, int64_t ts,
                         const std::string& from, const std::string& to, const std::string& r)
        : Event(id, seq, ts, "EquipmentStateChanged"),
          from_state(from), to_state(to), reason(r) {}

    std::string toJSON() const override;
};

// AI-detected degradation
struct DegradationDetected : public Event {
    std::string component;        // "bearing", "spindle", "tool"
    float degradation_pct;        // 0.0 - 100.0
    float predicted_mtbf_hours;   // Mean time before failure
    std::string ml_model_version;

    DegradationDetected(const std::string& id, int64_t seq, int64_t ts,
                       const std::string& comp, float deg, float mtbf, const std::string& model)
        : Event(id, seq, ts, "DegradationDetected"),
          component(comp), degradation_pct(deg), predicted_mtbf_hours(mtbf), ml_model_version(model) {}

    std::string toJSON() const override;
};

struct MaintenanceScheduled : public Event {
    int64_t scheduled_time_us;
    std::string maintenance_type;  // "preventive", "predictive", "corrective"
    std::string reason;

    MaintenanceScheduled(const std::string& id, int64_t seq, int64_t ts,
                        int64_t sched, const std::string& type, const std::string& r)
        : Event(id, seq, ts, "MaintenanceScheduled"),
          scheduled_time_us(sched), maintenance_type(type), reason(r) {}

    std::string toJSON() const override;
};

struct MaintenanceCompleted : public Event {
    std::string work_performed;
    int32_t duration_minutes;

    MaintenanceCompleted(const std::string& id, int64_t seq, int64_t ts,
                        const std::string& work, int32_t dur)
        : Event(id, seq, ts, "MaintenanceCompleted"),
          work_performed(work), duration_minutes(dur) {}

    std::string toJSON() const override;
};

// ============================================================================
// Equipment Health FSM State
// ============================================================================

enum class EquipmentState {
    HEALTHY,
    DEGRADING,
    CRITICAL,
    FAILED,
    MAINTENANCE,
    CALIBRATING
};

inline std::string stateToString(EquipmentState state) {
    switch(state) {
        case EquipmentState::HEALTHY: return "HEALTHY";
        case EquipmentState::DEGRADING: return "DEGRADING";
        case EquipmentState::CRITICAL: return "CRITICAL";
        case EquipmentState::FAILED: return "FAILED";
        case EquipmentState::MAINTENANCE: return "MAINTENANCE";
        case EquipmentState::CALIBRATING: return "CALIBRATING";
        default: return "UNKNOWN";
    }
}

inline EquipmentState stringToState(const std::string& s) {
    if (s == "HEALTHY") return EquipmentState::HEALTHY;
    if (s == "DEGRADING") return EquipmentState::DEGRADING;
    if (s == "CRITICAL") return EquipmentState::CRITICAL;
    if (s == "FAILED") return EquipmentState::FAILED;
    if (s == "MAINTENANCE") return EquipmentState::MAINTENANCE;
    if (s == "CALIBRATING") return EquipmentState::CALIBRATING;
    return EquipmentState::HEALTHY;
}

// Equipment health aggregate state
struct EquipmentHealthState {
    std::string equipment_id;
    EquipmentState current_state = EquipmentState::HEALTHY;
    int64_t last_sequence = 0;

    // Health metrics (derived from telemetry events)
    float max_vibration_rms = 0.0f;
    float max_temperature_c = 0.0f;
    float avg_motor_current_a = 0.0f;
    float tool_wear_mm = 0.0f;
    int32_t total_cycles = 0;
    int32_t telemetry_count = 0;

    // Degradation tracking
    std::optional<float> degradation_pct;
    std::optional<float> predicted_mtbf_hours;

    // Maintenance tracking
    std::optional<int64_t> maintenance_scheduled_time;
    int32_t maintenance_count = 0;

    // Snapshot metadata
    int64_t snapshot_sequence = 0;
    int64_t snapshot_timestamp_us = 0;

    // Apply event to update state (deterministic)
    void apply(const Event& event);

    // Create snapshot of current state
    std::string toSnapshotJSON() const;

    // Restore state from snapshot
    static EquipmentHealthState fromSnapshotJSON(const std::string& json);
};

// ============================================================================
// Commands (requests to change state)
// ============================================================================

struct Command {
    std::string entity_id;
    std::string command_type;

    Command(const std::string& id, const std::string& type)
        : entity_id(id), command_type(type) {}

    virtual ~Command() = default;
};

struct ReportTelemetry : public Command {
    float vibration_rms;
    float temperature_c;
    float motor_current_a;
    float spindle_speed_rpm;
    float tool_wear_mm;

    ReportTelemetry(const std::string& id, float vib, float temp, float curr, float speed, float wear)
        : Command(id, "ReportTelemetry"),
          vibration_rms(vib), temperature_c(temp), motor_current_a(curr),
          spindle_speed_rpm(speed), tool_wear_mm(wear) {}
};

struct ScheduleMaintenance : public Command {
    int64_t scheduled_time_us;
    std::string maintenance_type;
    std::string reason;

    ScheduleMaintenance(const std::string& id, int64_t time, const std::string& type, const std::string& r)
        : Command(id, "ScheduleMaintenance"),
          scheduled_time_us(time), maintenance_type(type), reason(r) {}
};

struct CompleteMaintenance : public Command {
    std::string work_performed;
    int32_t duration_minutes;

    CompleteMaintenance(const std::string& id, const std::string& work, int32_t dur)
        : Command(id, "CompleteMaintenance"),
          work_performed(work), duration_minutes(dur) {}
};

// ============================================================================
// Helper Functions
// ============================================================================

inline int64_t currentTimeMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

} // namespace event_sourcing

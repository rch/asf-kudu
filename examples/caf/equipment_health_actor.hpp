// Equipment Health Actor - Event Sourced FSM
//
// Demonstrates full event sourcing pattern:
// - Command validation → Event generation
// - Deterministic state rebuild from events
// - Snapshot creation (copy-on-write, zero-cost)
// - Recovery from Kudu (snapshot + event replay)
// - High-frequency telemetry processing
//
// Actor Messages:
// - Commands: ReportTelemetry, ScheduleMaintenance, CompleteMaintenance
// - Queries: GetState, GetSnapshot
// - Recovery: LoadSnapshot, ReplayEvents
// - Internal: PersistEvent, CreateSnapshot

#pragma once

#include "event_sourcing.hpp"
#include <caf/event_based_actor.hpp>
#include <caf/actor.hpp>
#include <memory>
#include <queue>

namespace equipment_health {

using namespace caf;
using namespace event_sourcing;

// ============================================================================
// Actor State
// ============================================================================

struct EquipmentHealthActorState {
    // Core event-sourced state
    EquipmentHealthState state;

    // Actor references
    actor aeron_bridge;  // For persisting events to Kudu

    // Event buffer (for batching)
    std::queue<std::shared_ptr<Event>> pending_events;

    // Snapshot configuration
    int64_t snapshot_interval = 1000;  // Create snapshot every N events
    int64_t events_since_snapshot = 0;

    // Performance metrics
    int64_t total_events_processed = 0;
    int64_t total_commands_received = 0;
    int64_t recovery_start_time_us = 0;
    int64_t recovery_end_time_us = 0;

    // Default constructor (CAF requires this)
    EquipmentHealthActorState() = default;

    EquipmentHealthActorState(const std::string& equipment_id, actor bridge)
        : aeron_bridge(bridge) {
        state.equipment_id = equipment_id;
    }
};

// ============================================================================
// Command Processing
// ============================================================================

// Process ReportTelemetry command → Generate TelemetryReported event
inline std::shared_ptr<Event> processReportTelemetry(
    EquipmentHealthActorState& actor_state,
    const ReportTelemetry& cmd) {

    int64_t seq = actor_state.state.last_sequence + 1;
    int64_t ts = currentTimeMicros();

    // Increment cycle count (simplified - in production, track separately)
    int32_t cycles = actor_state.state.total_cycles + 1;

    return std::make_shared<TelemetryReported>(
        cmd.entity_id, seq, ts,
        cmd.vibration_rms, cmd.temperature_c, cmd.motor_current_a,
        cmd.spindle_speed_rpm, cmd.tool_wear_mm, cycles
    );
}

// Process ScheduleMaintenance command → Generate MaintenanceScheduled event
inline std::shared_ptr<Event> processScheduleMaintenance(
    EquipmentHealthActorState& actor_state,
    const ScheduleMaintenance& cmd) {

    // Validation: Don't schedule if already in maintenance
    if (actor_state.state.current_state == EquipmentState::MAINTENANCE) {
        return nullptr;  // Command rejected
    }

    int64_t seq = actor_state.state.last_sequence + 1;
    int64_t ts = currentTimeMicros();

    return std::make_shared<MaintenanceScheduled>(
        cmd.entity_id, seq, ts,
        cmd.scheduled_time_us, cmd.maintenance_type, cmd.reason
    );
}

// Process CompleteMaintenance command → Generate MaintenanceCompleted + StateChanged events
inline std::vector<std::shared_ptr<Event>> processCompleteMaintenance(
    EquipmentHealthActorState& actor_state,
    const CompleteMaintenance& cmd) {

    std::vector<std::shared_ptr<Event>> events;

    int64_t ts = currentTimeMicros();

    // Event 1: MaintenanceCompleted
    int64_t seq1 = actor_state.state.last_sequence + 1;
    events.push_back(std::make_shared<MaintenanceCompleted>(
        cmd.entity_id, seq1, ts, cmd.work_performed, cmd.duration_minutes
    ));

    // Event 2: StateChanged (MAINTENANCE → CALIBRATING)
    int64_t seq2 = seq1 + 1;
    events.push_back(std::make_shared<EquipmentStateChanged>(
        cmd.entity_id, seq2, ts,
        stateToString(actor_state.state.current_state),
        stateToString(EquipmentState::CALIBRATING),
        "Maintenance completed, starting calibration"
    ));

    return events;
}

// ============================================================================
// Actor Behavior
// ============================================================================

behavior equipment_health_actor(stateful_actor<EquipmentHealthActorState>* self,
                                const std::string& equipment_id,
                                actor aeron_bridge);

} // namespace equipment_health

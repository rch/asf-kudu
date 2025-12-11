// Equipment Health Actor Implementation

#include "equipment_health_actor.hpp"
#include <caf/anon_mail.hpp>
#include <iostream>
#include <sstream>

namespace equipment_health {

using namespace caf;
using namespace event_sourcing;

// ============================================================================
// Helper: Persist Event to Kudu via Aeron Bridge
// ============================================================================

void persistEvent(stateful_actor<EquipmentHealthActorState>* self,
                  std::shared_ptr<Event> event) {
    // Build Kudu insert request
    std::ostringstream oss;
    oss << "{\"op\":\"insert\""
        << ",\"table\":\"equipment_events\""
        << ",\"entity_id\":\"" << event->entity_id << "\""
        << ",\"sequence\":\"" << event->sequence << "\""
        << ",\"timestamp\":\"" << event->timestamp_us << "\""
        << ",\"event_type\":\"" << event->event_type << "\""
        << ",\"event_data\":\"" << event->toJSON() << "\"}";

    std::string request = oss.str();

    // Create response handler
    auto response_handler = self->spawn([=](event_based_actor* handler_self) -> behavior {
        return {
            [=](const std::string& tag, const std::string& response) {
                if (tag != "kudu_response") return;

                if (response.find("\"status\":\"ok\"") != std::string::npos) {
                    handler_self->println("Event persisted: {} seq {}",
                                         event->event_type, event->sequence);
                } else {
                    handler_self->println("Persist failed: {}", response);
                }
                handler_self->quit();
            }
        };
    });

    // Send to Aeron bridge
    anon_mail("kudu_request", request, response_handler).send(self->state().aeron_bridge);
}

// ============================================================================
// Helper: Create Snapshot (Zero-Cost via Copy-on-Write)
// ============================================================================

void createSnapshot(stateful_actor<EquipmentHealthActorState>* self) {
    auto& state = self->state().state;

    // Update snapshot metadata
    state.snapshot_sequence = state.last_sequence;
    state.snapshot_timestamp_us = currentTimeMicros();

    // Serialize snapshot (copy-on-write - state continues to be used)
    std::string snapshot_json = state.toSnapshotJSON();

    self->println("Creating snapshot at sequence {} ({} bytes)",
                  state.snapshot_sequence, snapshot_json.size());

    // Build Kudu insert request for snapshot
    std::ostringstream oss;
    oss << "{\"op\":\"insert\""
        << ",\"table\":\"equipment_snapshots\""
        << ",\"entity_id\":\"" << state.equipment_id << "\""
        << ",\"sequence\":\"" << state.snapshot_sequence << "\""
        << ",\"timestamp\":\"" << state.snapshot_timestamp_us << "\""
        << ",\"snapshot_data\":\"" << snapshot_json << "\"}";

    std::string request = oss.str();

    // Create response handler
    auto response_handler = self->spawn([=](event_based_actor* handler_self) -> behavior {
        return {
            [=](const std::string& tag, const std::string& response) {
                if (tag != "kudu_response") return;

                if (response.find("\"status\":\"ok\"") != std::string::npos) {
                    handler_self->println("Snapshot saved: seq {}", state.snapshot_sequence);
                } else {
                    handler_self->println("Snapshot save failed: {}", response);
                }
                handler_self->quit();
            }
        };
    });

    // Send to Aeron bridge (async, non-blocking)
    anon_mail("kudu_request", request, response_handler).send(self->state().aeron_bridge);

    // Reset snapshot counter
    self->state().events_since_snapshot = 0;
}

// ============================================================================
// Actor Behavior - Message Handlers
// ============================================================================

behavior equipment_health_actor(stateful_actor<EquipmentHealthActorState>* self,
                                const std::string& equipment_id,
                                actor aeron_bridge) {

    // Initialize actor state
    self->state() = EquipmentHealthActorState(equipment_id, aeron_bridge);

    self->println("EquipmentHealthActor[{}]: Started", equipment_id);

    return {
        // ====================================================================
        // Command: ReportTelemetry (high frequency - 10Hz per machine)
        // ====================================================================
        [=](const std::string& tag, float vib, float temp, float curr, float speed, float wear) mutable {
            if (tag != "report_telemetry") return;

            self->state().total_commands_received++;

            // Create command
            ReportTelemetry cmd(equipment_id, vib, temp, curr, speed, wear);

            // Process command → generate event
            auto event = processReportTelemetry(self->state(), cmd);

            // Apply event to local state (optimistic - don't wait for Kudu)
            self->state().state.apply(*event);
            self->state().total_events_processed++;
            self->state().events_since_snapshot++;

            // Persist event asynchronously
            persistEvent(self, event);

            // Check if snapshot needed
            if (self->state().events_since_snapshot >= self->state().snapshot_interval) {
                createSnapshot(self);
            }
        },

        // ====================================================================
        // Command: ScheduleMaintenance
        // ====================================================================
        [=](const std::string& tag, int64_t scheduled_time,
            const std::string& maint_type, const std::string& reason) mutable {
            if (tag != "schedule_maintenance") return;

            self->state().total_commands_received++;

            // Create command
            ScheduleMaintenance cmd(equipment_id, scheduled_time, maint_type, reason);

            // Process command → generate event (may be rejected)
            auto event = processScheduleMaintenance(self->state(), cmd);

            if (!event) {
                self->println("EquipmentHealthActor[{}]: Maintenance scheduling rejected - already in maintenance",
                             equipment_id);
                return;
            }

            // Apply and persist
            self->state().state.apply(*event);
            self->state().total_events_processed++;
            self->state().events_since_snapshot++;

            persistEvent(self, event);

            self->println("EquipmentHealthActor[{}]: Maintenance scheduled for {}",
                         equipment_id, scheduled_time);
        },

        // ====================================================================
        // Command: CompleteMaintenance
        // ====================================================================
        [=](const std::string& tag, const std::string& work, int32_t duration) mutable {
            if (tag != "complete_maintenance") return;

            self->state().total_commands_received++;

            // Create command
            CompleteMaintenance cmd(equipment_id, work, duration);

            // Process command → generate multiple events
            auto events = processCompleteMaintenance(self->state(), cmd);

            // Apply and persist all events
            for (auto& event : events) {
                self->state().state.apply(*event);
                self->state().total_events_processed++;
                self->state().events_since_snapshot++;
                persistEvent(self, event);
            }

            self->println("EquipmentHealthActor[{}]: Maintenance completed - {} ({}m)",
                         equipment_id, work, duration);

            // Check if snapshot needed
            if (self->state().events_since_snapshot >= self->state().snapshot_interval) {
                createSnapshot(self);
            }
        },

        // ====================================================================
        // Query: GetState (for monitoring/debugging)
        // ====================================================================
        [=](const std::string& tag) -> std::string {
            if (tag != "get_state") {
                return "ERROR: Unknown query";
            }

            std::ostringstream oss;
            oss << "EquipmentHealth[" << equipment_id << "]: "
                << "state=" << stateToString(self->state().state.current_state)
                << ", seq=" << self->state().state.last_sequence
                << ", cycles=" << self->state().state.total_cycles
                << ", events=" << self->state().total_events_processed
                << ", commands=" << self->state().total_commands_received;

            if (self->state().state.degradation_pct) {
                oss << ", degradation=" << *self->state().state.degradation_pct << "%";
            }

            return oss.str();
        },

        // ====================================================================
        // Recovery: LoadSnapshot
        // ====================================================================
        [=](const std::string& tag, const std::string& snapshot_json) mutable {
            if (tag != "load_snapshot") return;

            self->println("EquipmentHealthActor[{}]: Loading snapshot...", equipment_id);

            self->state().recovery_start_time_us = currentTimeMicros();

            // Restore state from snapshot
            self->state().state = EquipmentHealthState::fromSnapshotJSON(snapshot_json);

            self->println("EquipmentHealthActor[{}]: Snapshot loaded - seq {}",
                         equipment_id, self->state().state.last_sequence);
        },

        // ====================================================================
        // Recovery: ReplayEvent (called for each event after snapshot)
        // ====================================================================
        [=](const std::string& tag, const std::string& event_type,
            int64_t seq, int64_t ts, const std::string& event_data) mutable {
            if (tag != "replay_event") return;

            // Reconstruct event and apply (simplified - production would deserialize fully)
            if (event_type == "TelemetryReported") {
                // Parse telemetry data (simplified)
                TelemetryReported event(equipment_id, seq, ts, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0);
                self->state().state.apply(event);
            }
            else if (event_type == "MaintenanceCompleted") {
                MaintenanceCompleted event(equipment_id, seq, ts, "unknown", 0);
                self->state().state.apply(event);
            }
            // ... handle other event types

            self->state().total_events_processed++;
        },

        // ====================================================================
        // Recovery: RecoveryComplete
        // ====================================================================
        [=](const std::string& tag) mutable {
            if (tag != "recovery_complete") return;

            self->state().recovery_end_time_us = currentTimeMicros();

            int64_t recovery_time_ms = (self->state().recovery_end_time_us -
                                       self->state().recovery_start_time_us) / 1000;

            self->println("EquipmentHealthActor[{}]: Recovery complete! Seq: {}, Events: {}, Time: {}ms",
                         equipment_id,
                         self->state().state.last_sequence,
                         self->state().total_events_processed,
                         recovery_time_ms);
        },

        // ====================================================================
        // Admin: GetMetrics
        // ====================================================================
        [=](const std::string& tag) -> std::string {
            if (tag != "get_metrics") {
                return "ERROR: Unknown query";
            }

            std::ostringstream oss;
            oss << "{"
                << "\"equipment_id\":\"" << equipment_id << "\","
                << "\"state\":\"" << stateToString(self->state().state.current_state) << "\","
                << "\"sequence\":" << self->state().state.last_sequence << ","
                << "\"total_events\":" << self->state().total_events_processed << ","
                << "\"total_commands\":" << self->state().total_commands_received << ","
                << "\"telemetry_count\":" << self->state().state.telemetry_count << ","
                << "\"total_cycles\":" << self->state().state.total_cycles << ","
                << "\"snapshot_seq\":" << self->state().state.snapshot_sequence;

            if (self->state().recovery_end_time_us > 0) {
                int64_t recovery_ms = (self->state().recovery_end_time_us -
                                      self->state().recovery_start_time_us) / 1000;
                oss << ",\"recovery_time_ms\":" << recovery_ms;
            }

            oss << "}";
            return oss.str();
        }
    };
}

} // namespace equipment_health

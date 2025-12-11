// Recovery Coordinator Implementation

#include "recovery_coordinator.hpp"
#include <caf/anon_mail.hpp>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace recovery {

using namespace caf;
using namespace equipment_health;

// ============================================================================
// Helper: Query Kudu for Latest Snapshots
// ============================================================================

void queryLatestSnapshot(stateful_actor<RecoveryCoordinatorState>* self,
                        const std::string& entity_id) {
    self->println("Recovery[{}]: Querying latest snapshot...", entity_id);

    // Build Kudu scan request for latest snapshot
    std::ostringstream oss;
    oss << "{\"op\":\"scan\""
        << ",\"table\":\"equipment_snapshots\""
        << ",\"entity_id\":\"" << entity_id << "\""
        << ",\"limit\":1"
        << ",\"order\":\"desc\"}";  // Latest snapshot first

    std::string request = oss.str();

    // Create response handler that sends message to coordinator (proper message-passing!)
    auto coordinator = actor_cast<actor>(self);
    auto response_handler = self->spawn([=](event_based_actor* handler_self) -> behavior {
        return {
            [=](const std::string& tag, const std::string& response) {
                if (tag != "kudu_response") return;

                if (response.find("\"status\":\"ok\"") != std::string::npos) {
                    // Parse snapshot data (simplified - production would use JSON library)
                    size_t pos = response.find("\"snapshot_data\":\"");
                    if (pos != std::string::npos) {
                        pos += 17;
                        size_t end = response.find("\"", pos);
                        std::string snapshot_json = response.substr(pos, end - pos);

                        // Extract snapshot sequence
                        int64_t snapshot_sequence = 0;
                        size_t seq_pos = response.find("\"sequence\":");
                        if (seq_pos != std::string::npos) {
                            seq_pos += 11;
                            snapshot_sequence = std::stoll(response.substr(seq_pos));
                        }

                        // Extract snapshot timestamp
                        int64_t snapshot_timestamp_us = 0;
                        size_t ts_pos = response.find("\"timestamp\":");
                        if (ts_pos != std::string::npos) {
                            ts_pos += 12;
                            snapshot_timestamp_us = std::stoll(response.substr(ts_pos));
                        }

                        handler_self->println("Recovery[{}]: Snapshot loaded - seq {}",
                                            entity_id, snapshot_sequence);

                        // Send message to coordinator (NO shared state access!)
                        anon_mail("snapshot_loaded", entity_id, snapshot_json,
                                 snapshot_sequence, snapshot_timestamp_us).send(coordinator);
                    }
                } else {
                    handler_self->println("Recovery[{}]: No snapshot found, replaying from beginning",
                                         entity_id);
                    // Send message to coordinator with empty snapshot
                    anon_mail("snapshot_loaded", entity_id, std::string(""),
                             int64_t(0), int64_t(0)).send(coordinator);
                }

                handler_self->quit();
            }
        };
    });

    // Send to Aeron bridge
    anon_mail("kudu_request", request, response_handler).send(self->state().aeron_bridge);
}

// ============================================================================
// Helper: Query Kudu for Events Since Snapshot
// ============================================================================

void queryEventsSinceSnapshot(stateful_actor<RecoveryCoordinatorState>* self,
                             const std::string& entity_id,
                             int64_t snapshot_sequence) {
    self->println("Recovery[{}]: Querying events since sequence {}...",
                 entity_id, snapshot_sequence);

    // Build Kudu scan request for events after snapshot
    std::ostringstream oss;
    oss << "{\"op\":\"scan\""
        << ",\"table\":\"equipment_events\""
        << ",\"entity_id\":\"" << entity_id << "\""
        << ",\"min_sequence\":\"" << (snapshot_sequence + 1) << "\""
        << ",\"order\":\"asc\"}";  // Chronological order

    std::string request = oss.str();

    // Create response handler that sends message to coordinator (proper message-passing!)
    auto coordinator = actor_cast<actor>(self);
    auto entity_actor = self->state().entities[entity_id].entity_actor;

    auto response_handler = self->spawn([=](event_based_actor* handler_self) -> behavior {
        return {
            [=](const std::string& tag, const std::string& response) {
                if (tag != "kudu_response") return;

                if (response.find("\"status\":\"ok\"") != std::string::npos) {
                    // Parse event stream (simplified - production would batch)
                    // In production, Aeron would stream events in batches of 100-1000

                    // Count events replayed (parse "rows":[...] array)
                    size_t rows_start = response.find("\"rows\":[");
                    int event_count = 0;

                    if (rows_start != std::string::npos) {
                        // Count comma-separated rows (simplified)
                        size_t pos = rows_start;
                        while ((pos = response.find("{", pos + 1)) != std::string::npos) {
                            event_count++;

                            // Extract event details and send to actor
                            // (simplified - production would properly parse JSON)
                            size_t type_pos = response.find("\"event_type\":\"", pos);
                            size_t seq_pos = response.find("\"sequence\":", pos);
                            size_t ts_pos = response.find("\"timestamp\":", pos);

                            if (type_pos != std::string::npos && seq_pos != std::string::npos) {
                                type_pos += 14;
                                size_t type_end = response.find("\"", type_pos);
                                std::string event_type = response.substr(type_pos, type_end - type_pos);

                                seq_pos += 11;
                                int64_t seq = std::stoll(response.substr(seq_pos));

                                ts_pos += 12;
                                int64_t ts = std::stoll(response.substr(ts_pos));

                                // Send replay event to entity actor
                                anon_mail("replay_event", event_type, seq, ts, "").send(entity_actor);
                            }
                        }

                        handler_self->println("Recovery[{}]: Replayed {} events",
                                            entity_id, event_count);
                    }

                    // Send message to coordinator (NO shared state access!)
                    anon_mail("events_loaded", entity_id, event_count).send(coordinator);
                } else {
                    handler_self->println("Recovery[{}]: Event query failed: {}",
                                         entity_id, response);
                    // Send message with 0 events
                    anon_mail("events_loaded", entity_id, 0).send(coordinator);
                }

                handler_self->quit();
            }
        };
    });

    // Send to Aeron bridge
    anon_mail("kudu_request", request, response_handler).send(self->state().aeron_bridge);
}

// ============================================================================
// Helper: Start Next Parallel Recovery
// ============================================================================

void startNextRecovery(stateful_actor<RecoveryCoordinatorState>* self) {
    auto& state = self->state();

    // Check if we can start more recoveries
    while (state.active_recoveries < state.max_parallel_recoveries &&
           !state.pending_entities.empty()) {

        std::string entity_id = state.pending_entities.back();
        state.pending_entities.pop_back();

        auto& entity = state.entities[entity_id];
        entity.start_time_us = event_sourcing::currentTimeMicros();
        entity.phase = RecoveryPhase::DISCOVERY;

        state.active_recoveries++;

        self->println("Recovery: Starting recovery for {} ({} active, {} pending)",
                     entity_id, state.active_recoveries, state.pending_entities.size());

        // Spawn equipment health actor
        entity.entity_actor = self->spawn(equipment_health_actor, entity_id, state.aeron_bridge);

        // Start recovery by querying latest snapshot
        queryLatestSnapshot(self, entity_id);
    }
}

// ============================================================================
// Helper: Complete Entity Recovery
// ============================================================================

void completeEntityRecovery(stateful_actor<RecoveryCoordinatorState>* self,
                           const std::string& entity_id) {
    auto& state = self->state();
    auto& entity = state.entities[entity_id];

    entity.end_time_us = event_sourcing::currentTimeMicros();
    entity.phase = RecoveryPhase::COMPLETE;

    int64_t recovery_time_ms = entity.getTotalRecoveryTimeMs();

    state.entities_recovered++;
    state.active_recoveries--;

    // Update global stats
    state.min_recovery_time_ms = std::min(state.min_recovery_time_ms, recovery_time_ms);
    state.max_recovery_time_ms = std::max(state.max_recovery_time_ms, recovery_time_ms);
    state.total_recovery_time_ms += recovery_time_ms;

    self->println("Recovery[{}]: Complete! Time: {}ms (snapshot: {}ms, replay: {}ms, events: {})",
                 entity_id,
                 recovery_time_ms,
                 entity.getSnapshotLoadTimeMs(),
                 entity.getReplayTimeMs(),
                 entity.events_replayed);

    // Start next recovery in pipeline
    startNextRecovery(self);

    // Check if all recoveries complete
    if (state.entities_recovered == state.total_entities) {
        state.recovery_end_time_us = event_sourcing::currentTimeMicros();

        self->println("\n╔══════════════════════════════════════════════════════════════╗");
        self->println("║  PARALLEL RECOVERY COMPLETE                                  ║");
        self->println("╚══════════════════════════════════════════════════════════════╝");
        self->println("Total entities:        {}", state.total_entities);
        self->println("Entities recovered:    {}", state.entities_recovered);
        self->println("Total events replayed: {}", state.total_events_replayed);
        self->println("Total time:            {}ms", state.getTotalRecoveryTimeMs());
        self->println("Throughput:            {:.2f} entities/sec", state.getRecoveryThroughput());
        self->println("Recovery time (min):   {}ms", state.min_recovery_time_ms);
        self->println("Recovery time (avg):   {:.2f}ms", state.getAverageRecoveryTimeMs());
        self->println("Recovery time (max):   {}ms", state.max_recovery_time_ms);
        self->println("══════════════════════════════════════════════════════════════\n");
    }
}

// ============================================================================
// Recovery Coordinator Behavior
// ============================================================================

behavior recovery_coordinator_actor(stateful_actor<RecoveryCoordinatorState>* self,
                                    actor aeron_bridge,
                                    const std::vector<std::string>& entity_ids) {

    // Initialize state
    self->state() = RecoveryCoordinatorState(aeron_bridge);
    self->state().total_entities = entity_ids.size();

    // Create entity recovery state for each entity
    for (const auto& entity_id : entity_ids) {
        EntityRecoveryState entity_state;
        entity_state.entity_id = entity_id;
        self->state().entities[entity_id] = entity_state;
        self->state().pending_entities.push_back(entity_id);
    }

    // Reverse pending_entities so we pop from the back efficiently
    std::reverse(self->state().pending_entities.begin(), self->state().pending_entities.end());

    self->println("\n╔══════════════════════════════════════════════════════════════╗");
    self->println("║  PARALLEL RECOVERY COORDINATOR                               ║");
    self->println("╚══════════════════════════════════════════════════════════════╝");
    self->println("Total entities:        {}", self->state().total_entities);
    self->println("Max parallel:          {}", self->state().max_parallel_recoveries);
    self->println("══════════════════════════════════════════════════════════════\n");

    self->state().recovery_start_time_us = event_sourcing::currentTimeMicros();

    // Start initial batch of parallel recoveries
    startNextRecovery(self);

    return {
        // ====================================================================
        // Command: SetMaxParallelRecoveries (tune concurrency)
        // ====================================================================
        [=](const std::string& tag, int32_t max_parallel) mutable {
            if (tag != "set_max_parallel") return;

            self->state().max_parallel_recoveries = max_parallel;
            self->println("Recovery: Max parallel recoveries set to {}", max_parallel);

            // Potentially start more recoveries if we increased the limit
            startNextRecovery(self);
        },

        // ====================================================================
        // Message: SnapshotLoaded (from response handler)
        // ====================================================================
        [=](const std::string& tag, const std::string& entity_id,
            const std::string& snapshot_json, int64_t snapshot_sequence,
            int64_t snapshot_timestamp_us) mutable {
            if (tag != "snapshot_loaded") return;

            auto& state = self->state();
            auto& entity = state.entities[entity_id];

            entity.snapshot_sequence = snapshot_sequence;
            entity.snapshot_timestamp_us = snapshot_timestamp_us;
            entity.snapshot_loaded_time_us = event_sourcing::currentTimeMicros();
            entity.phase = RecoveryPhase::SNAPSHOT_LOADING;

            // Send snapshot to entity actor
            if (!snapshot_json.empty()) {
                anon_mail("load_snapshot", snapshot_json).send(entity.entity_actor);
            }

            // Move to event replay phase
            entity.phase = RecoveryPhase::EVENT_REPLAY;
            queryEventsSinceSnapshot(self, entity_id, snapshot_sequence);
        },

        // ====================================================================
        // Message: EventsLoaded (from response handler)
        // ====================================================================
        [=](const std::string& tag, const std::string& entity_id,
            int event_count) mutable {
            if (tag != "events_loaded") return;

            auto& state = self->state();
            auto& entity = state.entities[entity_id];

            entity.events_replayed = event_count;
            state.total_events_replayed += event_count;
            entity.replay_complete_time_us = event_sourcing::currentTimeMicros();
            entity.phase = RecoveryPhase::VALIDATION;

            self->println("Recovery[{}]: Replayed {} events", entity_id, event_count);

            // Notify actor that recovery is complete
            anon_mail("recovery_complete").send(entity.entity_actor);

            // Mark entity as recovered
            completeEntityRecovery(self, entity_id);
        },

        // ====================================================================
        // Query Handler: Handle both get_recovery_status and get_metrics
        // (CAF can't distinguish between two lambdas with same signature)
        // ====================================================================
        [=](const std::string& tag) {
            std::string response;

            if (tag == "get_recovery_status") {
                std::ostringstream oss;
                oss << "{\n"
                    << "  \"total_entities\": " << self->state().total_entities << ",\n"
                    << "  \"entities_recovered\": " << self->state().entities_recovered << ",\n"
                    << "  \"active_recoveries\": " << self->state().active_recoveries << ",\n"
                    << "  \"pending_entities\": " << self->state().pending_entities.size() << ",\n"
                    << "  \"total_events_replayed\": " << self->state().total_events_replayed << ",\n";

                if (self->state().recovery_end_time_us > 0) {
                    oss << "  \"total_time_ms\": " << self->state().getTotalRecoveryTimeMs() << ",\n"
                        << "  \"throughput_entities_per_sec\": " << self->state().getRecoveryThroughput() << ",\n"
                        << "  \"min_recovery_ms\": " << self->state().min_recovery_time_ms << ",\n"
                        << "  \"avg_recovery_ms\": " << self->state().getAverageRecoveryTimeMs() << ",\n"
                        << "  \"max_recovery_ms\": " << self->state().max_recovery_time_ms << ",\n"
                        << "  \"status\": \"COMPLETE\"\n";
                } else {
                    int64_t elapsed_ms = (event_sourcing::currentTimeMicros() -
                                         self->state().recovery_start_time_us) / 1000;
                    oss << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
                        << "  \"status\": \"IN_PROGRESS\"\n";
                }

                oss << "}";
                response = oss.str();
            } else if (tag == "get_metrics") {
                auto metrics = getRecoveryMetrics(self->state());
                response = metrics.toJSON();
            } else {
                response = "ERROR: Unknown query tag: " + tag;
            }

            return response;
        }
    };
}

// ============================================================================
// Recovery Metrics Report
// ============================================================================

std::string RecoveryMetrics::toJSON() const {
    std::ostringstream oss;
    oss << "{\n"
        << "  \"total_entities\": " << total_entities << ",\n"
        << "  \"entities_recovered\": " << entities_recovered << ",\n"
        << "  \"total_events_replayed\": " << total_events_replayed << ",\n"
        << "  \"total_time_ms\": " << total_time_ms << ",\n"
        << "  \"throughput_entities_per_sec\": " << throughput_entities_per_sec << ",\n"
        << "  \"min_recovery_ms\": " << min_recovery_ms << ",\n"
        << "  \"avg_recovery_ms\": " << avg_recovery_ms << ",\n"
        << "  \"max_recovery_ms\": " << max_recovery_ms << "\n"
        << "}";
    return oss.str();
}

RecoveryMetrics getRecoveryMetrics(const RecoveryCoordinatorState& state) {
    RecoveryMetrics metrics;
    metrics.total_entities = state.total_entities;
    metrics.entities_recovered = state.entities_recovered;
    metrics.total_events_replayed = state.total_events_replayed;
    metrics.total_time_ms = state.getTotalRecoveryTimeMs();
    metrics.throughput_entities_per_sec = state.getRecoveryThroughput();
    metrics.min_recovery_ms = state.min_recovery_time_ms;
    metrics.max_recovery_ms = state.max_recovery_time_ms;
    metrics.avg_recovery_ms = state.getAverageRecoveryTimeMs();
    return metrics;
}

} // namespace recovery

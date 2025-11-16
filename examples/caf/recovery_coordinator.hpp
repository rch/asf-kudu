// Parallel Recovery Coordinator
//
// Orchestrates parallel recovery of event-sourced actors:
// 1. Discovers all entities needing recovery
// 2. Loads snapshots in parallel (leveraging Kudu's parallelism)
// 3. Replays events since snapshot (batch streaming via Aeron)
// 4. Tracks recovery progress and metrics
//
// Key Performance Goals:
// - Target: < 1 second for 10K entities with 1M events
// - Parallelism: Recover N actors concurrently (N = CPU cores)
// - Batching: Stream events in batches of 100-1000
// - Pipelining: Load snapshot while replaying previous entity's events
//
// Recovery Phases:
// 1. DISCOVERY   - Query Kudu for list of entities
// 2. SNAPSHOT    - Load most recent snapshot for each entity
// 3. REPLAY      - Stream events from snapshot sequence to latest
// 4. VALIDATION  - Verify state consistency
// 5. READY       - Actor ready to process new commands

#pragma once

#include "equipment_health_actor.hpp"
#include <caf/event_based_actor.hpp>
#include <caf/actor.hpp>
#include <vector>
#include <map>
#include <chrono>

namespace recovery {

using namespace caf;
using namespace equipment_health;

// ============================================================================
// Recovery State Tracking
// ============================================================================

enum class RecoveryPhase {
    IDLE,
    DISCOVERY,
    SNAPSHOT_LOADING,
    EVENT_REPLAY,
    VALIDATION,
    COMPLETE
};

struct EntityRecoveryState {
    std::string entity_id;
    actor entity_actor;

    RecoveryPhase phase = RecoveryPhase::IDLE;

    // Snapshot info
    int64_t snapshot_sequence = 0;
    int64_t snapshot_timestamp_us = 0;

    // Replay progress
    int64_t events_replayed = 0;
    int64_t latest_sequence = 0;

    // Timing
    int64_t start_time_us = 0;
    int64_t snapshot_loaded_time_us = 0;
    int64_t replay_complete_time_us = 0;
    int64_t end_time_us = 0;

    int64_t getTotalRecoveryTimeMs() const {
        if (end_time_us == 0) return 0;
        return (end_time_us - start_time_us) / 1000;
    }

    int64_t getSnapshotLoadTimeMs() const {
        if (snapshot_loaded_time_us == 0) return 0;
        return (snapshot_loaded_time_us - start_time_us) / 1000;
    }

    int64_t getReplayTimeMs() const {
        if (replay_complete_time_us == 0) return 0;
        return (replay_complete_time_us - snapshot_loaded_time_us) / 1000;
    }
};

// ============================================================================
// Recovery Coordinator State
// ============================================================================

struct RecoveryCoordinatorState {
    actor aeron_bridge;

    // Recovery tracking
    std::map<std::string, EntityRecoveryState> entities;
    std::vector<std::string> pending_entities;  // Queue for parallel recovery

    int32_t max_parallel_recoveries = 10;  // CPU cores
    int32_t active_recoveries = 0;

    // Global metrics
    int64_t total_entities = 0;
    int64_t entities_recovered = 0;
    int64_t total_events_replayed = 0;

    int64_t recovery_start_time_us = 0;
    int64_t recovery_end_time_us = 0;

    // Performance stats
    int64_t min_recovery_time_ms = INT64_MAX;
    int64_t max_recovery_time_ms = 0;
    int64_t total_recovery_time_ms = 0;

    RecoveryCoordinatorState(actor bridge) : aeron_bridge(bridge) {}

    double getAverageRecoveryTimeMs() const {
        if (entities_recovered == 0) return 0.0;
        return static_cast<double>(total_recovery_time_ms) / entities_recovered;
    }

    int64_t getTotalRecoveryTimeMs() const {
        if (recovery_end_time_us == 0) return 0;
        return (recovery_end_time_us - recovery_start_time_us) / 1000;
    }

    double getRecoveryThroughput() const {
        int64_t time_ms = getTotalRecoveryTimeMs();
        if (time_ms == 0) return 0.0;
        return (static_cast<double>(entities_recovered) * 1000.0) / time_ms;
    }
};

// ============================================================================
// Recovery Coordinator Behavior
// ============================================================================

behavior recovery_coordinator_actor(stateful_actor<RecoveryCoordinatorState>* self,
                                    actor aeron_bridge,
                                    const std::vector<std::string>& entity_ids);

// ============================================================================
// Helper: Query Kudu for Latest Snapshots
// ============================================================================

void queryLatestSnapshot(stateful_actor<RecoveryCoordinatorState>* self,
                        const std::string& entity_id);

// ============================================================================
// Helper: Query Kudu for Events Since Snapshot
// ============================================================================

void queryEventsSinceSnapshot(stateful_actor<RecoveryCoordinatorState>* self,
                             const std::string& entity_id,
                             int64_t snapshot_sequence);

// ============================================================================
// Helper: Start Next Parallel Recovery
// ============================================================================

void startNextRecovery(stateful_actor<RecoveryCoordinatorState>* self);

// ============================================================================
// Helper: Complete Entity Recovery
// ============================================================================

void completeEntityRecovery(stateful_actor<RecoveryCoordinatorState>* self,
                           const std::string& entity_id);

// ============================================================================
// Recovery Metrics Report
// ============================================================================

struct RecoveryMetrics {
    int64_t total_entities;
    int64_t entities_recovered;
    int64_t total_events_replayed;
    int64_t total_time_ms;
    double throughput_entities_per_sec;
    int64_t min_recovery_ms;
    int64_t max_recovery_ms;
    double avg_recovery_ms;

    std::string toJSON() const;
};

RecoveryMetrics getRecoveryMetrics(const RecoveryCoordinatorState& state);

} // namespace recovery

// Aeron IPC Bridge for Real Kudu Communication
//
// This actor provides a bridge between CAF actors and the kudu_service_simple
// process via Aeron IPC (shared memory transport).
//
// Architecture:
// - CAF Actor → AeronIPCBridge (this) → Aeron IPC → kudu_service_simple → Kudu Cluster
//
// Protocol:
// - Request channel: aeron:ipc?stream-id=20 (CAF → kudu_service)
// - Response channel: aeron:ipc?stream-id=21 (kudu_service → CAF)
// - Message format: JSON (same as mock bridge for compatibility)
//
// Usage:
//   auto bridge = system.spawn(aeron_ipc_bridge_actor);
//   anon_mail("kudu_request", json_request, response_handler).send(bridge);
//
// The bridge maintains request tracking to route responses back to the correct
// response_handler actor.

#pragma once

#include <caf/event_based_actor.hpp>
#include <caf/actor.hpp>
#include <string>
#include <map>
#include <atomic>
#include <memory>

// Forward declare Aeron types properly without creating nested namespace
namespace aeron {
    class Aeron;
    class Publication;
    class Subscription;
}

namespace aeron_bridge {

using namespace caf;

// ============================================================================
// Request Tracking
// ============================================================================

struct PendingRequest {
    std::string request_id;      // Unique request ID (for correlation)
    std::string request_json;    // Original request (for retries)
    actor response_handler;      // Actor expecting the response
    int64_t sent_time_us;        // When request was sent
    int32_t retry_count;         // Number of retries attempted
};

// ============================================================================
// Aeron IPC Bridge State
// ============================================================================

struct AeronIPCBridgeState {
    // Aeron context
    std::shared_ptr<aeron::Aeron> aeron_context;
    std::shared_ptr<aeron::Publication> publication;      // For sending requests
    std::shared_ptr<aeron::Subscription> subscription;    // For receiving responses

    // Request tracking
    std::map<std::string, PendingRequest> pending_requests;
    std::atomic<uint64_t> next_request_id{1};

    // Configuration (must match kudu_service_simple)
    std::string request_channel = "aeron:ipc";
    int32_t request_stream_id = 20;     // kudu_service subscribes on stream 20
    std::string response_channel = "aeron:ipc";
    int32_t response_stream_id = 21;    // kudu_service publishes on stream 21

    int32_t request_timeout_ms = 5000;  // 5 second timeout
    int32_t max_retries = 3;

    // Statistics
    int64_t requests_sent = 0;
    int64_t responses_received = 0;
    int64_t timeouts = 0;
    int64_t errors = 0;

    // State
    bool connected = false;
    int64_t last_poll_time_us = 0;
};

// ============================================================================
// Actor Behavior
// ============================================================================

// Main actor behavior
behavior aeron_ipc_bridge_actor(stateful_actor<AeronIPCBridgeState>* self);

// ============================================================================
// Helper Functions
// ============================================================================

// Initialize Aeron context and channels
bool initializeAeron(stateful_actor<AeronIPCBridgeState>* self);

// Send request via Aeron IPC
bool sendRequest(stateful_actor<AeronIPCBridgeState>* self,
                const std::string& request_json,
                actor response_handler);

// Poll for responses from Aeron subscription
void pollResponses(stateful_actor<AeronIPCBridgeState>* self);

// Handle response message
void handleResponse(stateful_actor<AeronIPCBridgeState>* self,
                   const std::string& response_json);

// Check for timed-out requests
void checkTimeouts(stateful_actor<AeronIPCBridgeState>* self);

// Generate unique request ID
std::string generateRequestId(AeronIPCBridgeState& state);

// Inject request ID into JSON
std::string injectRequestId(const std::string& request_json, const std::string& request_id);

// Extract request ID from JSON response
std::string extractRequestId(const std::string& response_json);

} // namespace aeron_bridge

// Aeron IPC Bridge Implementation

#include "aeron_ipc_bridge.hpp"
#include <Aeron.h>
#include <caf/all.hpp>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace aeron_bridge {

using namespace caf;
using namespace aeron;

// ============================================================================
// Helper: Generate Unique Request ID
// ============================================================================

std::string generateRequestId(AeronIPCBridgeState& state) {
    uint64_t id = state.next_request_id.fetch_add(1);
    std::ostringstream oss;
    oss << "req_" << id;
    return oss.str();
}

// ============================================================================
// Helper: Inject Request ID into JSON
// ============================================================================

std::string injectRequestId(const std::string& request_json, const std::string& request_id) {
    // Find the opening brace and inject request_id field
    // Simple approach: insert after first '{'
    size_t pos = request_json.find('{');
    if (pos == std::string::npos) {
        return request_json;  // Invalid JSON, return as-is
    }

    std::string modified = request_json.substr(0, pos + 1);
    modified += "\"request_id\":\"" + request_id + "\",";
    modified += request_json.substr(pos + 1);

    return modified;
}

// ============================================================================
// Helper: Extract Request ID from JSON Response
// ============================================================================

std::string extractRequestId(const std::string& response_json) {
    size_t pos = response_json.find("\"request_id\":\"");
    if (pos == std::string::npos) return "";

    pos += 14;  // Length of "\"request_id\":\"" (was 15 - off-by-one error)
    size_t end = response_json.find("\"", pos);
    if (end == std::string::npos) return "";

    return response_json.substr(pos, end - pos);
}

// ============================================================================
// Initialize Aeron Context and Channels
// ============================================================================

bool initializeAeron(stateful_actor<AeronIPCBridgeState>* self) {
    auto& state = self->state();

    try {
        // Create Aeron context
        aeron::Context context;
        state.aeron_context = aeron::Aeron::connect(context);

        if (!state.aeron_context) {
            self->println("ERROR: Failed to create Aeron context");
            return false;
        }

        // Create publication (for sending requests)
        std::string pub_channel = state.request_channel + "?stream-id=" + std::to_string(state.request_stream_id);
        int64_t pub_id = state.aeron_context->addPublication(pub_channel, state.request_stream_id);

        if (pub_id < 0) {
            self->println("ERROR: Failed to add publication to channel: {}", pub_channel);
            return false;
        }

        state.publication = state.aeron_context->findPublication(pub_id);

        // Wait for publication to be connected
        int retries = 100;
        while (!state.publication && retries-- > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            state.publication = state.aeron_context->findPublication(pub_id);
        }

        if (!state.publication) {
            self->println("ERROR: Publication not available after timeout");
            return false;
        }

        // Wait for publication to be connected to a subscriber
        self->println("Waiting for publication to connect to kudu_service...");
        retries = 100;  // 10 seconds total
        while (!state.publication->isConnected() && retries-- > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!state.publication->isConnected()) {
            self->println("WARNING: Publication not connected to any subscribers yet");
            self->println("  Make sure kudu-service process is running");
            // Don't fail - we'll retry on first send attempt
        } else {
            self->println("✓ Publication connected to subscriber");
        }

        // Create subscription (for receiving responses)
        std::string sub_channel = state.response_channel + "?stream-id=" + std::to_string(state.response_stream_id);
        int64_t sub_id = state.aeron_context->addSubscription(sub_channel, state.response_stream_id);

        if (sub_id < 0) {
            self->println("ERROR: Failed to add subscription to channel: {}", sub_channel);
            return false;
        }

        state.subscription = state.aeron_context->findSubscription(sub_id);

        // Wait for subscription to be connected
        retries = 100;
        while (!state.subscription && retries-- > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            state.subscription = state.aeron_context->findSubscription(sub_id);
        }

        if (!state.subscription) {
            self->println("ERROR: Subscription not connected after timeout");
            return false;
        }

        self->println("✓ Aeron IPC Bridge initialized");
        self->println("  Request channel:  {} (stream {})", state.request_channel, state.request_stream_id);
        self->println("  Response channel: {} (stream {})", state.response_channel, state.response_stream_id);

        state.connected = true;
        return true;

    } catch (const std::exception& e) {
        self->println("ERROR: Exception initializing Aeron: {}", e.what());
        return false;
    }
}

// ============================================================================
// Send Request via Aeron IPC
// ============================================================================

bool sendRequest(stateful_actor<AeronIPCBridgeState>* self,
                const std::string& request_json,
                actor response_handler) {
    auto& state = self->state();

    if (!state.connected || !state.publication) {
        self->println("ERROR: Aeron not connected, cannot send request");
        state.errors++;
        return false;
    }

    // Wait for publication to be connected if it's not yet
    int wait_retries = 50;  // 5 seconds max
    while (!state.publication->isConnected() && wait_retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!state.publication->isConnected()) {
        self->println("ERROR: Publication not connected after waiting");
        self->println("  Make sure kudu-service process is running");
        state.errors++;
        return false;
    }

    // Generate unique request ID
    std::string request_id = generateRequestId(state);

    // Inject request ID into JSON
    std::string request_with_id = injectRequestId(request_json, request_id);

    // Debug: Log first request to verify injection
    static int debug_count = 0;
    if (debug_count++ < 3) {
        self->println("DEBUG: Sending request with ID '{}': {}", request_id, request_with_id.substr(0, 200));
    }

    // Send via Aeron
    aeron::concurrent::logbuffer::BufferClaim buffer_claim;
    int64_t position = state.publication->tryClaim(request_with_id.length(), buffer_claim);

    if (position < 0) {
        self->println("ERROR: Failed to claim buffer for request (position: {})", position);
        if (position == -1) {
            self->println("  NOT_CONNECTED: Subscriber (kudu-service) disconnected");
        } else if (position == -2) {
            self->println("  BACK_PRESSURED: Subscriber is slow");
        }
        state.errors++;
        return false;
    }

    // Copy data to buffer
    buffer_claim.buffer().putBytes(buffer_claim.offset(),
                                    reinterpret_cast<const uint8_t*>(request_with_id.data()),
                                    request_with_id.length());
    buffer_claim.commit();

    // Track pending request
    PendingRequest pending;
    pending.request_id = request_id;
    pending.request_json = request_json;
    pending.response_handler = response_handler;
    pending.sent_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pending.retry_count = 0;

    state.pending_requests[request_id] = pending;
    state.requests_sent++;

    return true;
}

// ============================================================================
// Poll for Responses from Aeron Subscription
// ============================================================================

void pollResponses(stateful_actor<AeronIPCBridgeState>* self) {
    auto& state = self->state();

    if (!state.connected || !state.subscription) {
        return;
    }

    // Fragment handler - processes received messages
    auto fragment_handler = [self](concurrent::AtomicBuffer& buffer, util::index_t offset, util::index_t length, aeron::Header& header) {
        // Extract message data
        std::string response_json(reinterpret_cast<const char*>(buffer.buffer() + offset), length);

        // Handle the response
        handleResponse(self, response_json);
    };

    // Poll subscription (process up to 10 fragments per poll)
    state.subscription->poll(fragment_handler, 10);
}

// ============================================================================
// Handle Response Message
// ============================================================================

void handleResponse(stateful_actor<AeronIPCBridgeState>* self,
                   const std::string& response_json) {
    auto& state = self->state();

    // Extract request ID from response
    std::string request_id = extractRequestId(response_json);

    if (request_id.empty()) {
        // Check if response has request_id field at all
        if (response_json.find("request_id") == std::string::npos) {
            self->println("WARNING: Received response without request_id field: {}", response_json.substr(0, 150));
        } else {
            self->println("WARNING: Found request_id field but failed to extract it: {}", response_json.substr(0, 150));
        }
        return;
    }

    // Find pending request
    auto it = state.pending_requests.find(request_id);
    if (it == state.pending_requests.end()) {
        self->println("WARNING: Received response for unknown request_id: {}", request_id);
        return;
    }

    PendingRequest& pending = it->second;

    // Send response to handler
    anon_mail("kudu_response", response_json).send(pending.response_handler);

    // Remove from pending
    state.pending_requests.erase(it);
    state.responses_received++;
}

// ============================================================================
// Check for Timed-Out Requests
// ============================================================================

void checkTimeouts(stateful_actor<AeronIPCBridgeState>* self) {
    auto& state = self->state();

    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::vector<std::string> timed_out_ids;

    for (auto& pair : state.pending_requests) {
        PendingRequest& pending = pair.second;

        int64_t elapsed_ms = (now_us - pending.sent_time_us) / 1000;

        if (elapsed_ms > state.request_timeout_ms) {
            if (pending.retry_count < state.max_retries) {
                // Retry the request
                self->println("Retrying timed-out request: {} (attempt {})",
                             pending.request_id, pending.retry_count + 1);

                pending.retry_count++;
                pending.sent_time_us = now_us;

                // Resend (reuse request_id for correlation)
                std::string request_with_id = injectRequestId(pending.request_json, pending.request_id);

                aeron::concurrent::logbuffer::BufferClaim buffer_claim;
                int64_t position = state.publication->tryClaim(request_with_id.length(), buffer_claim);

                if (position >= 0) {
                    buffer_claim.buffer().putBytes(buffer_claim.offset(),
                                                    reinterpret_cast<const uint8_t*>(request_with_id.data()),
                                                    request_with_id.length());
                    buffer_claim.commit();
                } else {
                    self->println("ERROR: Failed to retry request {}", pending.request_id);
                }

            } else {
                // Max retries exceeded - send error response
                self->println("ERROR: Request timed out after {} retries: {}",
                             state.max_retries, pending.request_id);

                std::string error_response = "{\"status\":\"error\",\"message\":\"Request timed out\"}";
                anon_mail("kudu_response", error_response).send(pending.response_handler);

                timed_out_ids.push_back(pending.request_id);
                state.timeouts++;
            }
        }
    }

    // Remove timed-out requests
    for (const auto& id : timed_out_ids) {
        state.pending_requests.erase(id);
    }
}

// ============================================================================
// Actor Behavior
// ============================================================================

behavior aeron_ipc_bridge_actor(stateful_actor<AeronIPCBridgeState>* self) {
    // Initialize Aeron on startup
    if (!initializeAeron(self)) {
        self->println("FATAL: Failed to initialize Aeron IPC Bridge");
        self->quit(exit_reason::user_shutdown);
        return {};
    }

    // Start polling timer (poll every 10ms)
    self->send(self, "poll");

    return {
        // Handle incoming requests
        [=](const std::string& tag, const std::string& request, actor response_handler) {
            if (tag != "kudu_request") return;

            sendRequest(self, request, response_handler);
        },

        // Periodic polling for responses and timeouts
        [=](const std::string& tag) {
            if (tag != "poll") return;

            pollResponses(self);
            checkTimeouts(self);

            // Schedule next poll
            self->delayed_send(self, std::chrono::milliseconds(10), "poll");
        },

        // Get statistics
        [=](const std::string& tag) -> result<std::string> {
            if (tag != "get_stats") return {};

            auto& state = self->state();
            std::ostringstream oss;
            oss << "{\"requests_sent\":" << state.requests_sent
                << ",\"responses_received\":" << state.responses_received
                << ",\"pending\":" << state.pending_requests.size()
                << ",\"timeouts\":" << state.timeouts
                << ",\"errors\":" << state.errors
                << ",\"connected\":" << (state.connected ? "true" : "false")
                << "}";
            return oss.str();
        }
    };
}

} // namespace aeron_bridge

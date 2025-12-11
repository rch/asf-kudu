// Phase 2: CAF Event Coordinator with Aeron IPC
//
// This is a CAF actor-based coordinator that:
// - Uses CAF actors for business logic and event sourcing
// - Communicates with Kudu service via Aeron IPC (NO Kudu client code)
// - Demonstrates actor-based event sourcing pattern
//
// NO KUDU DEPENDENCIES - Pure CAF + Aeron to avoid TLS conflicts
//
// Architecture:
// - EventSourcedActor: Manages entity state using event sourcing
// - AeronBridge: Handles Aeron IPC communication with Kudu service
// - Coordinator: Orchestrates actors and processes commands
//

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/anon_mail.hpp>
#include <caf/init_global_meta_objects.hpp>

#include <Aeron.h>

#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <thread>
#include <sstream>
#include <memory>

using namespace caf;
using namespace aeron;

namespace {
  std::atomic<bool> running{true};

  void sigint_handler(int) {
    running = false;
  }

  // Simple JSON builder for requests
  std::string buildKuduRequest(const std::string& op, const std::string& params) {
    return "{\"op\":\"" + op + "\"" + params + "}";
  }

  // Parse JSON response status
  bool isSuccessResponse(const std::string& json) {
    return json.find("\"status\":\"ok\"") != std::string::npos;
  }

  std::string extractMessage(const std::string& json) {
    size_t pos = json.find("\"message\":\"");
    if (pos == std::string::npos) return "";
    pos += 11;
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
  }
}

// Aeron Bridge Actor - handles Aeron IPC communication
behavior aeron_bridge_actor(event_based_actor* self,
                            std::shared_ptr<Publication> pub,
                            std::shared_ptr<Subscription> sub) {

  // Response handler map: request_id -> response_handler_actor
  std::map<int, actor> pending_requests;
  int next_request_id = 0;

  // Buffer for Aeron messages
  auto response_buffer = std::make_shared<AtomicBuffer>(new std::uint8_t[8192], 8192);

  // Poll for Aeron responses periodically
  self->run_delayed(std::chrono::milliseconds(10), [=]() mutable {
    if (!running) return;

    fragment_handler_t handler = [=](AtomicBuffer& buffer, util::index_t offset,
                                      util::index_t length, Header&) mutable {
      std::string response(
          reinterpret_cast<const char*>(buffer.buffer() + offset),
          static_cast<std::size_t>(length));

      self->println("AeronBridge: Got response: {}", response.substr(0, 100));

      // Broadcast response to all pending requests (simplified for Phase 2)
      for (auto& [id, handler] : pending_requests) {
        anon_mail("kudu_response", response).send(handler);
      }
      pending_requests.clear();
    };

    sub->poll(handler, 10);

    // Reschedule
    self->run_delayed(std::chrono::milliseconds(10), [=]() mutable {
      if (running) {
        // This creates a polling loop
      }
    });
  });

  return {
    [=](const std::string& tag, const std::string& request, actor response_handler) mutable -> result<void> {
      if (tag != "kudu_request") {
        return make_error(sec::unexpected_message);
      }

      self->println("AeronBridge: Sending request: {}", request);

      // Send via Aeron
      response_buffer->putStringWithoutLength(0, request);
      std::int64_t result = pub->offer(*response_buffer, 0,
                                       static_cast<util::index_t>(request.length()));

      if (result > 0) {
        int req_id = next_request_id++;
        pending_requests[req_id] = response_handler;
        self->println("AeronBridge: Request sent (pos: {}, id: {})", result, req_id);
      } else {
        self->println("AeronBridge: Failed to send request: {}", result);
        anon_mail("kudu_response", "{\"status\":\"error\",\"message\":\"Failed to send\"}").send(response_handler);
      }
      return unit;
    }
  };
}

// Event Sourced Entity Actor
behavior event_sourced_entity_actor(event_based_actor* self,
                                    const std::string& entity_id,
                                    actor aeron_bridge) {

  int64_t next_sequence = 1;
  std::string table_name = "test_events";

  return {
    [=](const std::string& tag, const std::string& event_type, const std::string& event_data) mutable {
      if (tag != "apply_event") {
        return;
      }

      self->println("Entity[{}]: Applying event: {} (seq {})", entity_id, event_type, next_sequence);

      // Build insert request
      std::ostringstream params;
      params << ",\"table\":\"" << table_name << "\""
             << ",\"entity_id\":\"" << entity_id << "\""
             << ",\"sequence\":\"" << next_sequence << "\""
             << ",\"event_type\":\"" << event_type << "\""
             << ",\"event_data\":\"" << event_data << "\"";

      std::string request = buildKuduRequest("insert", params.str());

      // Create response handler
      auto response_handler = self->spawn([=](event_based_actor* handler_self) -> behavior {
        return {
          [=](const std::string& tag, const std::string& response) {
            if (tag != "kudu_response") return;

            if (isSuccessResponse(response)) {
              handler_self->println("Entity[{}]: Event {} persisted successfully", entity_id, event_type);
            } else {
              handler_self->println("Entity[{}]: Failed to persist event: {}",
                                   entity_id, extractMessage(response));
            }
            handler_self->quit();
          }
        };
      });

      // Send request via Aeron bridge
      anon_mail("kudu_request", request, response_handler).send(aeron_bridge);
      next_sequence++;
    },

    [=](const std::string& tag) mutable {
      if (tag != "get_state") {
        return;
      }

      self->println("Entity[{}]: Current sequence: {}", entity_id, next_sequence);
    }
  };
}

// Coordinator Actor - orchestrates the demo
behavior coordinator_actor(event_based_actor* self, actor aeron_bridge) {

  // Spawn entity actors
  auto entity1 = self->spawn(event_sourced_entity_actor, "entity1", aeron_bridge);
  auto entity2 = self->spawn(event_sourced_entity_actor, "entity2", aeron_bridge);

  self->println("Coordinator: Entity actors spawned");

  // Demo sequence
  self->run_delayed(std::chrono::seconds(1), [=]() {
    self->println("\n=== Starting Event Sourcing Demo ===\n");

    // Step 1: Create table
    self->println("Step 1: Creating events table...");
    std::string create_req = buildKuduRequest("create_table", ",\"table\":\"test_events\"");

    auto create_handler = self->spawn([=](event_based_actor* h) -> behavior {
      return {
        [=](const std::string& tag, const std::string& response) {
          if (tag != "kudu_response") return;
          h->println("Table creation: {}", extractMessage(response));
          h->quit();
        }
      };
    });

    anon_mail("kudu_request", create_req, create_handler).send(aeron_bridge);
  });

  self->run_delayed(std::chrono::seconds(2), [=]() {
    // Step 2: Apply events to entity1
    self->println("\nStep 2: Applying events to entity1...");
    anon_mail("apply_event", "EntityCreated", "name=Alice").send(entity1);
    anon_mail("apply_event", "NameChanged", "name=AliceSmith").send(entity1);
    anon_mail("apply_event", "StateChanged", "active=true").send(entity1);
  });

  self->run_delayed(std::chrono::seconds(3), [=]() {
    // Step 3: Apply events to entity2
    self->println("\nStep 3: Applying events to entity2...");
    anon_mail("apply_event", "EntityCreated", "name=Bob").send(entity2);
    anon_mail("apply_event", "StateChanged", "active=false").send(entity2);
  });

  self->run_delayed(std::chrono::seconds(4), [=]() {
    // Step 4: Query entity state
    self->println("\nStep 4: Querying entity states...");
    anon_mail("get_state").send(entity1);
    anon_mail("get_state").send(entity2);
  });

  self->run_delayed(std::chrono::seconds(5), [=]() {
    // Step 5: Scan events from Kudu
    self->println("\nStep 5: Scanning events from Kudu...");

    std::string scan_req1 = buildKuduRequest("scan",
        ",\"table\":\"test_events\",\"entity_id\":\"entity1\"");
    std::string scan_req2 = buildKuduRequest("scan",
        ",\"table\":\"test_events\",\"entity_id\":\"entity2\"");

    auto scan_handler1 = self->spawn([](event_based_actor* h) -> behavior {
      return {
        [=](const std::string& tag, const std::string& response) {
          if (tag != "kudu_response") return;
          h->println("Entity1 events: {}", response.substr(0, 200));
          h->quit();
        }
      };
    });

    auto scan_handler2 = self->spawn([](event_based_actor* h) -> behavior {
      return {
        [=](const std::string& tag, const std::string& response) {
          if (tag != "kudu_response") return;
          h->println("Entity2 events: {}", response.substr(0, 200));
          h->quit();
        }
      };
    });

    anon_mail("kudu_request", scan_req1, scan_handler1).send(aeron_bridge);
    anon_mail("kudu_request", scan_req2, scan_handler2).send(aeron_bridge);
  });

  self->run_delayed(std::chrono::seconds(7), [=]() {
    self->println("\n=== Demo Complete ===\n");
    running = false;
  });

  return {
    [=](const std::string& tag) {
      self->println("Coordinator: Received: {}", tag);
    }
  };
}

int main() {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  std::cout << "CAF Event Coordinator (Phase 2) Starting..." << std::endl;

  try {
    // Initialize CAF
    caf::core::init_global_meta_objects();
    actor_system_config cfg;
    actor_system system{cfg};
    std::cout << "  ✓ CAF system initialized" << std::endl;

    // Connect to Aeron
    Context ctx;
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);
    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create publication for requests (stream 20)
    const std::string req_channel = "aeron:ipc";
    const std::int32_t req_stream = 20;

    std::int64_t pubId = aeron->addPublication(req_channel, req_stream);
    std::shared_ptr<Publication> publication;
    while (!publication && running) {
      publication = aeron->findPublication(pubId);
      if (!publication) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    std::cout << "  ✓ Publication ready (stream: " << req_stream << ")" << std::endl;

    // Create subscription for responses (stream 21)
    const std::string resp_channel = "aeron:ipc";
    const std::int32_t resp_stream = 21;

    std::int64_t subId = aeron->addSubscription(resp_channel, resp_stream);
    std::shared_ptr<Subscription> subscription;
    while (!subscription && running) {
      subscription = aeron->findSubscription(subId);
      if (!subscription) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    std::cout << "  ✓ Subscription ready (stream: " << resp_stream << ")" << std::endl;

    // Spawn Aeron bridge actor
    scoped_actor self{system};
    auto bridge = self->spawn(aeron_bridge_actor, publication, subscription);
    std::cout << "  ✓ Aeron bridge actor spawned" << std::endl;

    // Spawn coordinator actor
    auto coordinator = self->spawn(coordinator_actor, bridge);
    std::cout << "  ✓ Coordinator actor spawned" << std::endl;

    std::cout << std::endl;
    std::cout << "System ready! Running demo..." << std::endl;
    std::cout << std::endl;

    // Main loop - keep system alive
    while (running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << std::endl;
    std::cout << "Coordinator stopping..." << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Coordinator stopped." << std::endl;
  return 0;
}

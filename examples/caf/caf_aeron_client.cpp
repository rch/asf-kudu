// CAF + Aeron Integration - Client Side
//
// This demonstrates how to use CAF actors with Aeron for IPC instead of CAF I/O.
// The client sends requests via Aeron and receives responses.
//
// Architecture:
//   CAF Coordinator Actor -> Aeron Publisher -> [Service] -> Aeron Subscriber

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/anon_mail.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <Aeron.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sstream>

using namespace caf;
using namespace aeron;

namespace {
  std::atomic<bool> running{true};
  std::atomic<int> responsesReceived{0};

  void sigint_handler(int) {
    running = false;
  }

  // Coordinator actor that sends requests
  behavior coordinator_actor(event_based_actor* self,
                             std::shared_ptr<Publication> pub) {
    return {
      [self, pub](const std::string& tag, int id) {
        if (tag != "sendreq") {
          return;
        }
        std::string request = "Request " + std::to_string(id);
        self->println("Coordinator: Sending {}", request);

        // Send via Aeron
        AtomicBuffer buffer(new std::uint8_t[256], 256);
        buffer.putStringWithoutLength(0, request);

        std::int64_t result = pub->offer(
            buffer, 0,
            static_cast<util::index_t>(request.length()));

        if (result > 0) {
          self->println("Coordinator: Sent (position: {})", result);
        } else if (result == aeron::NOT_CONNECTED) {
          self->println("Coordinator: Service not connected yet");
        } else {
          self->println("Coordinator: Send failed: {}", result);
        }
      },

      [self](const std::string& tag, const std::string& response) {
        if (tag != "gotresp") {
          return;
        }
        self->println("Coordinator: Received response: {}", response);
        responsesReceived++;
      }
    };
  }
}

int main() {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  try {
    std::cout << "CAF + Aeron Client Starting..." << std::endl;

    // Initialize CAF
    caf::core::init_global_meta_objects();
    actor_system_config cfg;
    actor_system system{cfg};

    std::cout << "  ✓ CAF system initialized" << std::endl;

    // Create Aeron context (use default aeron dir)
    Context ctx;
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);
    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create publication for requests (stream 10 - matches service subscription)
    const std::string req_channel = "aeron:ipc";
    const std::int32_t req_stream = 10;

    std::int64_t pubId = aeron->addPublication(req_channel, req_stream);
    std::shared_ptr<Publication> publication;
    while (!publication && running) {
      publication = aeron->findPublication(pubId);
      if (!publication) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    if (!publication) {
      std::cerr << "Failed to create publication" << std::endl;
      return 1;
    }

    std::cout << "  ✓ Publication ready (stream: " << req_stream << ")" << std::endl;

    // Create subscription for responses (stream 11 - matches service publication)
    const std::string resp_channel = "aeron:ipc";
    const std::int32_t resp_stream = 11;

    std::int64_t subId = aeron->addSubscription(resp_channel, resp_stream);
    std::shared_ptr<Subscription> subscription;
    while (!subscription && running) {
      subscription = aeron->findSubscription(subId);
      if (!subscription) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    if (!subscription) {
      std::cerr << "Failed to create subscription" << std::endl;
      return 1;
    }

    std::cout << "  ✓ Subscription ready (stream: " << resp_stream << ")" << std::endl;

    // Create coordinator actor
    auto coordinator = system.spawn(coordinator_actor, publication);
    std::cout << "  ✓ Coordinator actor spawned" << std::endl;
    std::cout << std::endl;
    std::cout << "Client ready! Sending requests..." << std::endl;
    std::cout << std::endl;

    // Fragment handler for incoming responses
    fragment_handler_t handler = [&](AtomicBuffer& buffer, util::index_t offset,
                                      util::index_t length, Header& header) {
      // Extract response string
      std::string response(
          reinterpret_cast<const char*>(buffer.buffer() + offset),
          static_cast<std::size_t>(length));

      std::cout << "Aeron: Received response: " << response << std::endl;

      // Notify coordinator actor
      anon_mail("gotresp", response).send(coordinator);
    };

    // Send requests periodically
    int requestId = 0;
    auto lastSendTime = std::chrono::steady_clock::now();
    const auto sendInterval = std::chrono::seconds(1);

    while (running && requestId < 10) {
      // Poll for responses
      subscription->poll(handler, 10);

      // Send a request every second
      auto now = std::chrono::steady_clock::now();
      if (now - lastSendTime >= sendInterval) {
        anon_mail("sendreq", requestId).send(coordinator);
        requestId++;
        lastSendTime = now;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait a bit for final responses
    std::cout << std::endl;
    std::cout << "Waiting for final responses..." << std::endl;
    auto waitStart = std::chrono::steady_clock::now();
    while (responsesReceived < requestId &&
           std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(5)) {
      subscription->poll(handler, 10);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << std::endl;
    std::cout << "Client stopping..." << std::endl;
    std::cout << "  Requests sent: " << requestId << std::endl;
    std::cout << "  Responses received: " << responsesReceived << std::endl;

    // Cleanup
    anon_send_exit(coordinator, exit_reason::user_shutdown);

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Client stopped." << std::endl;
  return 0;
}

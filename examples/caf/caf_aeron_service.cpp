// CAF + Aeron Integration - Service Side
//
// This demonstrates how to use CAF actors with Aeron for IPC instead of CAF I/O.
// The service receives requests via Aeron and processes them with CAF actors.
//
// Architecture:
//   Aeron Subscriber -> CAF Worker Actor -> Aeron Publisher (for responses)

#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scoped_actor.hpp>
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

  void sigint_handler(int) {
    running = false;
  }

  // Worker actor that processes requests
  behavior worker_actor(event_based_actor* self) {
    return {
      [self](const std::string& tag, const std::string& request) -> result<std::string> {
        if (tag != "process") {
          return make_error(sec::unexpected_message);
        }
        self->println("Worker: Processing request: {}", request);

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Echo back with modification
        std::string response = "PROCESSED: " + request;
        self->println("Worker: Sending response: {}", response);

        return response;
      }
    };
  }
}

int main() {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  try {
    std::cout << "CAF + Aeron Service Starting..." << std::endl;

    // Initialize CAF
    caf::core::init_global_meta_objects();
    actor_system_config cfg;
    actor_system system{cfg};

    std::cout << "  ✓ CAF system initialized" << std::endl;

    // Create worker actor
    auto worker = system.spawn(worker_actor);
    std::cout << "  ✓ Worker actor spawned" << std::endl;

    // Create Aeron context (use default aeron dir)
    Context ctx;
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);
    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create subscription for requests (stream 10)
    const std::string req_channel = "aeron:ipc";
    const std::int32_t req_stream = 10;

    std::int64_t subId = aeron->addSubscription(req_channel, req_stream);
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

    std::cout << "  ✓ Subscription ready (stream: " << req_stream << ")" << std::endl;

    // Create publication for responses (stream 11)
    const std::string resp_channel = "aeron:ipc";
    const std::int32_t resp_stream = 11;

    std::int64_t pubId = aeron->addPublication(resp_channel, resp_stream);
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

    std::cout << "  ✓ Publication ready (stream: " << resp_stream << ")" << std::endl;
    std::cout << std::endl;
    std::cout << "Service ready! Waiting for requests (Ctrl+C to stop)..." << std::endl;
    std::cout << std::endl;

    // Fragment handler for incoming requests
    AtomicBuffer responseBuffer(new std::uint8_t[512], 512);

    fragment_handler_t handler = [&](AtomicBuffer& buffer, util::index_t offset,
                                      util::index_t length, Header& header) {
      // Extract request string
      std::string request(
          reinterpret_cast<const char*>(buffer.buffer() + offset),
          static_cast<std::size_t>(length));

      std::cout << "Aeron: Received request: " << request << std::endl;

      // Process with CAF actor
      scoped_actor self{system};
      self->mail("process", request)
        .request(worker, std::chrono::seconds(5))
        .receive(
          [&](const std::string& response) {
            std::cout << "CAF: Got response from worker: " << response << std::endl;

            // Send response via Aeron
            responseBuffer.putStringWithoutLength(0, response);
            std::int64_t result = publication->offer(
                responseBuffer, 0,
                static_cast<util::index_t>(response.length()));

            if (result > 0) {
              std::cout << "Aeron: Sent response (position: " << result << ")" << std::endl;
            } else {
              std::cout << "Aeron: Failed to send response: " << result << std::endl;
            }
          },
          [](const error& err) {
            std::cerr << "CAF: Worker error: " << to_string(err) << std::endl;
          }
        );
    };

    // Main service loop
    int requestsProcessed = 0;
    while (running) {
      int fragmentsRead = subscription->poll(handler, 10);

      if (fragmentsRead > 0) {
        requestsProcessed += fragmentsRead;
      }

      // Brief yield if no messages
      if (fragmentsRead == 0) {
        std::this_thread::yield();
      }
    }

    std::cout << std::endl;
    std::cout << "Service stopping... (processed " << requestsProcessed << " requests)" << std::endl;

    // Cleanup
    anon_send_exit(worker, exit_reason::user_shutdown);

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Service stopped." << std::endl;
  return 0;
}

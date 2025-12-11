// Simple Aeron IPC Subscriber Example
// Tests Aeron's shared memory IPC transport
//
// This subscriber receives messages via Aeron IPC (shared memory).
// Start this first, then run the publisher.

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <Aeron.h>

using namespace aeron;

namespace {
  std::atomic<bool> running{true};
  std::atomic<int> messageCount{0};

  void sigint_handler(int) {
    running = false;
  }

  // Fragment handler - called for each message received
  void fragmentHandler(
      AtomicBuffer& buffer,
      util::index_t offset,
      util::index_t length,
      Header& header) {

    // Extract message string
    std::string message(
        reinterpret_cast<const char*>(buffer.buffer() + offset),
        static_cast<std::size_t>(length));

    messageCount++;

    std::cout << "  ✓ Received: " << message
              << " (length: " << length
              << ", position: " << header.position()
              << ")" << std::endl;
  }
}

int main() {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  try {
    std::cout << "Aeron Subscriber Starting..." << std::endl;

    // Create Aeron context
    // IPC uses "aeron:ipc" channel (shared memory)
    // Use default aeronDir (will be /dev/shm/aeron-<username>)
    Context ctx;

    std::cout << "  Creating Aeron instance..." << std::endl;

    // Create Aeron instance
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);

    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create subscription on IPC channel
    // Must match publisher's channel and stream ID
    const std::string channel = "aeron:ipc";
    const std::int32_t streamId = 10;

    std::cout << "  Adding subscription..." << std::endl;
    std::cout << "    Channel: " << channel << std::endl;
    std::cout << "    Stream ID: " << streamId << std::endl;

    std::int64_t subId = aeron->addSubscription(channel, streamId);

    std::cout << "  Waiting for subscription to be ready..." << std::endl;

    // Wait for subscription to be ready
    std::shared_ptr<Subscription> subscription;
    while (!subscription && running) {
      subscription = aeron->findSubscription(subId);
      if (!subscription) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    if (!subscription) {
      std::cerr << "Failed to create subscription (interrupted)" << std::endl;
      return 1;
    }

    std::cout << "  ✓ Subscription ready" << std::endl;
    std::cout << std::endl;
    std::cout << "Waiting for messages (Ctrl+C to stop)..." << std::endl;
    std::cout << std::endl;

    // Poll for messages
    fragment_handler_t handler = fragmentHandler;

    while (running) {
      // Poll subscription for messages
      // Returns number of fragments read
      int fragmentsRead = subscription->poll(handler, 10);

      if (fragmentsRead == 0) {
        // No messages available, sleep briefly
        std::this_thread::yield();
      }
    }

    std::cout << std::endl;
    std::cout << "Subscriber stopping... (received " << messageCount << " messages)" << std::endl;

    // Cleanup happens automatically via RAII

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Subscriber stopped." << std::endl;
  return 0;
}

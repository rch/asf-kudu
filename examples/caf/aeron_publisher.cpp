// Simple Aeron IPC Publisher Example
// Tests Aeron's shared memory IPC transport
//
// This publisher sends messages via Aeron IPC (shared memory).
// Run the subscriber first to receive messages.

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <Aeron.h>

using namespace aeron;

namespace {
  std::atomic<bool> running{true};

  void sigint_handler(int) {
    running = false;
  }
}

int main() {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  try {
    std::cout << "Aeron Publisher Starting..." << std::endl;

    // Create Aeron context
    // IPC uses "aeron:ipc" channel (shared memory)
    // Use default aeronDir (will be /dev/shm/aeron-<username>)
    Context ctx;

    std::cout << "  Creating Aeron instance..." << std::endl;

    // Create Aeron instance
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);

    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create publication on IPC channel
    // Channel: aeron:ipc (uses shared memory for ultra-low latency)
    // Stream ID: 10 (arbitrary ID to identify this stream)
    const std::string channel = "aeron:ipc";
    const std::int32_t streamId = 10;

    std::cout << "  Adding publication..." << std::endl;
    std::cout << "    Channel: " << channel << std::endl;
    std::cout << "    Stream ID: " << streamId << std::endl;

    std::int64_t pubId = aeron->addPublication(channel, streamId);

    std::cout << "  Waiting for publication to be ready..." << std::endl;

    // Wait for publication to be ready
    std::shared_ptr<Publication> publication;
    while (!publication && running) {
      publication = aeron->findPublication(pubId);
      if (!publication) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    if (!publication) {
      std::cerr << "Failed to create publication (interrupted)" << std::endl;
      return 1;
    }

    std::cout << "  ✓ Publication ready" << std::endl;
    std::cout << std::endl;
    std::cout << "Publishing messages (Ctrl+C to stop)..." << std::endl;
    std::cout << std::endl;

    // Publish messages
    int messageCount = 0;
    AtomicBuffer buffer(new std::uint8_t[256], 256);

    while (running) {
      // Create message
      std::string message = "Message " + std::to_string(messageCount);
      buffer.putStringWithoutLength(0, message);

      // Offer message to publication
      std::int64_t result = publication->offer(buffer, 0, static_cast<util::index_t>(message.length()));

      if (result > 0) {
        std::cout << "  ✓ Published: " << message << " (position: " << result << ")" << std::endl;
        messageCount++;

        // Publish one message per second
        std::this_thread::sleep_for(std::chrono::seconds(1));
      } else if (result == aeron::NOT_CONNECTED) {
        std::cout << "  ⚠ No subscribers connected yet..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      } else if (result == aeron::BACK_PRESSURED) {
        std::cout << "  ⚠ Back pressure, waiting..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      } else {
        std::cout << "  ✗ Offer failed: " << result << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    std::cout << std::endl;
    std::cout << "Publisher stopping... (sent " << messageCount << " messages)" << std::endl;

    // Cleanup happens automatically via RAII

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Publisher stopped." << std::endl;
  return 0;
}

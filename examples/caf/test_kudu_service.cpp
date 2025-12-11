// Phase 1 Test Client for Kudu Service
//
// Sends test requests to kudu_service_simple via Aeron IPC
// Tests: create_table, insert, scan operations

#include <Aeron.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <sstream>

using namespace aeron;

namespace {
  std::atomic<bool> running{true};

  void sigint_handler(int) {
    running = false;
  }

  std::string buildRequest(const std::string& op, const std::string& params) {
    return "{\"op\":\"" + op + "\"" + params + "}";
  }
}

int main() {
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  try {
    std::cout << "Kudu Service Test Client Starting..." << std::endl;

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
    std::cout << std::endl;

    // Test requests
    AtomicBuffer buffer(new std::uint8_t[1024], 1024);
    int responsesReceived = 0;

    fragment_handler_t handler = [&](AtomicBuffer& buf, util::index_t offset,
                                      util::index_t length, Header&) {
      std::string response(
          reinterpret_cast<const char*>(buf.buffer() + offset),
          static_cast<std::size_t>(length));
      std::cout << "  Response: " << response << std::endl;
      responsesReceived++;
    };

    // Test 1: Create table
    std::cout << "Test 1: Create events table" << std::endl;
    std::string req1 = buildRequest("create_table", ",\"table\":\"test_events\"");
    buffer.putStringWithoutLength(0, req1);
    publication->offer(buffer, 0, static_cast<util::index_t>(req1.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    // Test 2: Insert event 1
    std::cout << "\nTest 2: Insert event 1" << std::endl;
    std::string req2 = buildRequest("insert",
        ",\"table\":\"test_events\""
        ",\"entity_id\":\"entity1\""
        ",\"sequence\":\"1\""
        ",\"event_type\":\"EntityCreated\""
        ",\"event_data\":\"name=TestEntity\"");
    buffer.putStringWithoutLength(0, req2);
    publication->offer(buffer, 0, static_cast<util::index_t>(req2.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    // Test 3: Insert event 2
    std::cout << "\nTest 3: Insert event 2" << std::endl;
    std::string req3 = buildRequest("insert",
        ",\"table\":\"test_events\""
        ",\"entity_id\":\"entity1\""
        ",\"sequence\":\"2\""
        ",\"event_type\":\"StateChanged\""
        ",\"event_data\":\"value=42\"");
    buffer.putStringWithoutLength(0, req3);
    publication->offer(buffer, 0, static_cast<util::index_t>(req3.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    // Test 4: Insert event for entity2
    std::cout << "\nTest 4: Insert event for entity2" << std::endl;
    std::string req4 = buildRequest("insert",
        ",\"table\":\"test_events\""
        ",\"entity_id\":\"entity2\""
        ",\"sequence\":\"1\""
        ",\"event_type\":\"EntityCreated\""
        ",\"event_data\":\"name=AnotherEntity\"");
    buffer.putStringWithoutLength(0, req4);
    publication->offer(buffer, 0, static_cast<util::index_t>(req4.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    // Test 5: Scan events for entity1
    std::cout << "\nTest 5: Scan events for entity1" << std::endl;
    std::string req5 = buildRequest("scan",
        ",\"table\":\"test_events\""
        ",\"entity_id\":\"entity1\"");
    buffer.putStringWithoutLength(0, req5);
    publication->offer(buffer, 0, static_cast<util::index_t>(req5.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    // Test 6: Scan events for entity2
    std::cout << "\nTest 6: Scan events for entity2" << std::endl;
    std::string req6 = buildRequest("scan",
        ",\"table\":\"test_events\""
        ",\"entity_id\":\"entity2\"");
    buffer.putStringWithoutLength(0, req6);
    publication->offer(buffer, 0, static_cast<util::index_t>(req6.length()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    subscription->poll(handler, 10);

    std::cout << std::endl;
    std::cout << "Test complete!" << std::endl;
    std::cout << "Responses received: " << responsesReceived << "/6" << std::endl;

    if (responsesReceived == 6) {
      std::cout << "✓ All tests passed!" << std::endl;
      return 0;
    } else {
      std::cout << "✗ Some tests failed" << std::endl;
      return 1;
    }

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}

// Phase 1: Kudu Service with Aeron IPC
//
// This is a simple Kudu persistence service that:
// - Receives operation requests via Aeron IPC (JSON format)
// - Performs Kudu operations (create table, insert, scan)
// - Sends responses back via Aeron IPC
//
// NO CAF DEPENDENCIES - Pure Kudu + Aeron to avoid TLS conflicts
//
// Request format (JSON):
//   {"op": "create_table", "table": "events"}
//   {"op": "insert", "table": "events", "data": {"entity_id": "e1", "event": "created"}}
//   {"op": "scan", "table": "events", "entity_id": "e1"}
//
// Response format (JSON):
//   {"status": "ok", "result": {...}}
//   {"status": "error", "message": "..."}

#include <kudu/client/client.h>
#include <kudu/client/write_op.h>
#include <kudu/client/value.h>
#include <Aeron.h>

#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <thread>
#include <sstream>
#include <memory>

using namespace kudu;
using namespace kudu::client;
using namespace aeron;

namespace {
  std::atomic<bool> running{true};

  void sigint_handler(int) {
    running = false;
  }

  // Simple JSON parser/builder (minimal, for MVP)
  std::string extractField(const std::string& json, const std::string& field) {
    size_t pos = json.find("\"" + field + "\":");
    if (pos == std::string::npos) return "";

    pos = json.find("\"", pos + field.length() + 3);
    if (pos == std::string::npos) return "";

    size_t end = json.find("\"", pos + 1);
    if (end == std::string::npos) return "";

    return json.substr(pos + 1, end - pos - 1);
  }

  std::string buildResponse(const std::string& status, const std::string& message = "") {
    std::ostringstream oss;
    oss << "{\"status\":\"" << status << "\"";
    if (!message.empty()) {
      oss << ",\"message\":\"" << message << "\"";
    }
    oss << "}";
    return oss.str();
  }

  // Kudu operations
  class KuduService {
  public:
    explicit KuduService(const std::string& master_addrs) : master_addrs_(master_addrs) {}

    bool Connect() {
      KuduClientBuilder builder;
      builder.add_master_server_addr(master_addrs_);
      Status s = builder.Build(&client_);
      if (!s.ok()) {
        std::cerr << "Failed to connect to Kudu: " << s.ToString() << std::endl;
        return false;
      }
      std::cout << "✓ Connected to Kudu at " << master_addrs_ << std::endl;
      return true;
    }

    std::string CreateEventsTable(const std::string& table_name) {
      // Check if table exists
      bool exists = false;
      Status s = client_->TableExists(table_name, &exists);
      if (!s.ok()) {
        return buildResponse("error", "Failed to check table: " + s.ToString());
      }

      if (exists) {
        std::cout << "Table " << table_name << " already exists" << std::endl;
        return buildResponse("ok", "Table already exists");
      }

      // Create schema for events table
      KuduSchema schema;
      KuduSchemaBuilder b;
      b.AddColumn("entity_id")->Type(KuduColumnSchema::STRING)->NotNull();
      b.AddColumn("sequence")->Type(KuduColumnSchema::INT64)->NotNull();
      b.AddColumn("timestamp")->Type(KuduColumnSchema::INT64)->NotNull();
      b.AddColumn("event_type")->Type(KuduColumnSchema::STRING)->NotNull();
      b.AddColumn("event_data")->Type(KuduColumnSchema::STRING);
      b.SetPrimaryKey({"entity_id", "sequence"});

      s = b.Build(&schema);
      if (!s.ok()) {
        return buildResponse("error", "Failed to build schema: " + s.ToString());
      }

      // Create table
      std::unique_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
      s = table_creator->table_name(table_name)
          .schema(&schema)
          .add_hash_partitions({"entity_id"}, 4)
          .num_replicas(1)
          .Create();

      if (!s.ok()) {
        return buildResponse("error", "Failed to create table: " + s.ToString());
      }

      std::cout << "✓ Created table: " << table_name << std::endl;
      return buildResponse("ok", "Table created");
    }

    std::string InsertEvent(const std::string& table_name,
                           const std::string& entity_id,
                           int64_t sequence,
                           const std::string& event_type,
                           const std::string& event_data) {
      sp::shared_ptr<KuduTable> table;
      Status s = client_->OpenTable(table_name, &table);
      if (!s.ok()) {
        return buildResponse("error", "Failed to open table: " + s.ToString());
      }

      sp::shared_ptr<KuduSession> session = client_->NewSession();
      session->SetTimeoutMillis(10000);

      std::unique_ptr<KuduInsert> insert(table->NewInsert());
      KuduPartialRow* row = insert->mutable_row();

      s = row->SetString("entity_id", entity_id);
      if (!s.ok()) return buildResponse("error", "Failed to set entity_id");

      s = row->SetInt64("sequence", sequence);
      if (!s.ok()) return buildResponse("error", "Failed to set sequence");

      s = row->SetInt64("timestamp", time(nullptr));
      if (!s.ok()) return buildResponse("error", "Failed to set timestamp");

      s = row->SetString("event_type", event_type);
      if (!s.ok()) return buildResponse("error", "Failed to set event_type");

      s = row->SetString("event_data", event_data);
      if (!s.ok()) return buildResponse("error", "Failed to set event_data");

      s = session->Apply(insert.release());
      if (!s.ok()) {
        return buildResponse("error", "Failed to apply insert: " + s.ToString());
      }

      s = session->Flush();
      if (!s.ok()) {
        return buildResponse("error", "Failed to flush: " + s.ToString());
      }

      std::cout << "✓ Inserted event: " << entity_id << "/" << sequence << std::endl;
      return buildResponse("ok", "Event inserted");
    }

    std::string ScanEvents(const std::string& table_name, const std::string& entity_id) {
      sp::shared_ptr<KuduTable> table;
      Status s = client_->OpenTable(table_name, &table);
      if (!s.ok()) {
        return buildResponse("error", "Failed to open table: " + s.ToString());
      }

      KuduScanner scanner(table.get());

      // Filter by entity_id
      KuduPredicate* pred = table->NewComparisonPredicate(
          "entity_id",
          KuduPredicate::EQUAL,
          KuduValue::CopyString(entity_id));
      s = scanner.AddConjunctPredicate(pred);
      if (!s.ok()) {
        return buildResponse("error", "Failed to add predicate: " + s.ToString());
      }

      s = scanner.Open();
      if (!s.ok()) {
        return buildResponse("error", "Failed to open scanner: " + s.ToString());
      }

      int count = 0;
      std::ostringstream result;
      result << "{\"status\":\"ok\",\"events\":[";

      KuduScanBatch batch;
      while (scanner.HasMoreRows()) {
        s = scanner.NextBatch(&batch);
        if (!s.ok()) {
          return buildResponse("error", "Failed to get next batch: " + s.ToString());
        }

        for (const KuduScanBatch::RowPtr& row : batch) {
          if (count > 0) result << ",";

          Slice entity;
          int64_t seq, ts;
          Slice ev_type, ev_data;

          s = row.GetString("entity_id", &entity);
          s = row.GetInt64("sequence", &seq);
          s = row.GetInt64("timestamp", &ts);
          s = row.GetString("event_type", &ev_type);
          s = row.GetString("event_data", &ev_data);

          result << "{\"entity_id\":\"" << entity.ToString() << "\""
                 << ",\"sequence\":" << seq
                 << ",\"timestamp\":" << ts
                 << ",\"event_type\":\"" << ev_type.ToString() << "\""
                 << ",\"event_data\":\"" << ev_data.ToString() << "\"}";
          count++;
        }
      }

      result << "],\"count\":" << count << "}";
      std::cout << "✓ Scanned " << count << " events for " << entity_id << std::endl;
      return result.str();
    }

  private:
    std::string master_addrs_;
    sp::shared_ptr<KuduClient> client_;
  };
}

int main(int argc, char* argv[]) {
  // Install signal handler
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  // Parse master addresses
  std::string master_addrs = "127.0.0.1:7051";
  if (argc > 1) {
    master_addrs = argv[1];
  }

  std::cout << "Kudu Service (Phase 1) Starting..." << std::endl;
  std::cout << "  Kudu master: " << master_addrs << std::endl;

  try {
    // Initialize Kudu service
    KuduService kudu_service(master_addrs);
    if (!kudu_service.Connect()) {
      std::cerr << "Failed to connect to Kudu" << std::endl;
      return 1;
    }

    // Pre-create event sourcing tables
    std::cout << "  Creating event sourcing tables..." << std::endl;
    std::string result = kudu_service.CreateEventsTable("equipment_events");
    if (result.find("\"status\":\"ok\"") != std::string::npos) {
      std::cout << "  ✓ equipment_events table ready" << std::endl;
    } else {
      std::cout << "  ! equipment_events: " << result << std::endl;
    }

    result = kudu_service.CreateEventsTable("equipment_snapshots");
    if (result.find("\"status\":\"ok\"") != std::string::npos) {
      std::cout << "  ✓ equipment_snapshots table ready" << std::endl;
    } else {
      std::cout << "  ! equipment_snapshots: " << result << std::endl;
    }

    // Create Aeron context
    Context ctx;
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);
    std::cout << "  ✓ Connected to Aeron" << std::endl;

    // Create subscription for requests (stream 20)
    const std::string req_channel = "aeron:ipc";
    const std::int32_t req_stream = 20;

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

    // Create publication for responses (stream 21)
    const std::string resp_channel = "aeron:ipc";
    const std::int32_t resp_stream = 21;

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

    // Response buffer
    AtomicBuffer responseBuffer(new std::uint8_t[8192], 8192);
    int requestsProcessed = 0;

    // Fragment handler for incoming requests
    fragment_handler_t handler = [&](AtomicBuffer& buffer, util::index_t offset,
                                      util::index_t length, Header&) {
      // Extract request
      std::string request(
          reinterpret_cast<const char*>(buffer.buffer() + offset),
          static_cast<std::size_t>(length));

      // Debug: Only show first 200 chars to avoid spam
      std::cout << "Request: " << request.substr(0, 200) << (request.length() > 200 ? "..." : "") << std::endl;

      // Extract request_id (CRITICAL for response correlation)
      std::string request_id = extractField(request, "request_id");

      // Debug: Log request_id extraction for first few requests
      static int req_count = 0;
      if (req_count++ < 5) {
        std::cout << "  Extracted request_id: '" << request_id << "'" << std::endl;
      }

      // Parse operation
      std::string op = extractField(request, "op");
      std::string response;

      if (op == "create_table") {
        std::string table = extractField(request, "table");
        response = kudu_service.CreateEventsTable(table);
      } else if (op == "insert") {
        std::string table = extractField(request, "table");
        std::string entity_id = extractField(request, "entity_id");
        std::string seq_str = extractField(request, "sequence");
        std::string event_type = extractField(request, "event_type");
        std::string event_data = extractField(request, "event_data");

        int64_t sequence = std::stoll(seq_str);
        response = kudu_service.InsertEvent(table, entity_id, sequence, event_type, event_data);
      } else if (op == "scan") {
        std::string table = extractField(request, "table");
        std::string entity_id = extractField(request, "entity_id");
        response = kudu_service.ScanEvents(table, entity_id);
      } else {
        response = buildResponse("error", "Unknown operation: " + op);
      }

      // Inject request_id into response for correlation
      if (!request_id.empty()) {
        // Insert request_id after opening brace
        size_t pos = response.find('{');
        if (pos != std::string::npos) {
          response.insert(pos + 1, "\"request_id\":\"" + request_id + "\",");

          // Debug: Log response for first few requests
          static int resp_count = 0;
          if (resp_count++ < 5) {
            std::cout << "  Response with request_id: " << response.substr(0, 200) << std::endl;
          }
        }
      } else {
        std::cout << "  WARNING: No request_id to inject into response!" << std::endl;
      }

      // Send response
      responseBuffer.putStringWithoutLength(0, response);
      std::int64_t result = publication->offer(
          responseBuffer, 0,
          static_cast<util::index_t>(response.length()));

      if (result > 0) {
        std::cout << "Response: " << response.substr(0, 80) << "..." << std::endl;
        requestsProcessed++;
      } else {
        std::cout << "Failed to send response: " << result << std::endl;
      }
    };

    // Main service loop
    while (running) {
      int fragmentsRead = subscription->poll(handler, 10);

      if (fragmentsRead == 0) {
        std::this_thread::yield();
      }
    }

    std::cout << std::endl;
    std::cout << "Service stopping... (processed " << requestsProcessed << " requests)" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "Service stopped." << std::endl;
  return 0;
}

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Phase 4: Kudu Service Process (Process Separation Solution)
// This process owns the Kudu client and handles all Kudu operations
// Communicates with CAF clients via CAF I/O (network/typed actors)
// NO CAF actors in this process - just uses CAF for networking

#include <caf/all.hpp>
#include <caf/io/all.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "kudu/client/client.h"
#include "kudu/client/row_result.h"
#include "kudu/client/stubs.h"
#include "kudu/client/value.h"
#include "kudu/client/write_op.h"
#include "kudu/common/partial_row.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"

using namespace caf;
using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::KuduColumnSchema;
using kudu::client::KuduInsert;
using kudu::client::KuduScanBatch;
using kudu::client::KuduScanner;
using kudu::client::KuduSchema;
using kudu::client::KuduSchemaBuilder;
using kudu::client::KuduSession;
using kudu::client::KuduTable;
using kudu::client::KuduTableCreator;
using kudu::client::sp::shared_ptr;
using kudu::KuduPartialRow;
using kudu::MonoDelta;
using kudu::Status;

static const char* kTableName = "caf_phase4_test";

// Message types for communication
CAF_BEGIN_TYPE_ID_BLOCK(kudu_service, caf::first_custom_type_id)
  CAF_ADD_ATOM(kudu_service, init_atom)
  CAF_ADD_ATOM(kudu_service, insert_atom)
  CAF_ADD_ATOM(kudu_service, scan_atom)
  CAF_ADD_ATOM(kudu_service, cleanup_atom)
  CAF_ADD_ATOM(kudu_service, done_atom)
CAF_END_TYPE_ID_BLOCK(kudu_service)

// Define the typed interface for the Kudu service
using kudu_service_actor = typed_actor<
  // init(master_addrs) -> done | error
  result<done_atom>(init_atom, std::string),
  // insert(num_rows) -> done | error
  result<done_atom>(insert_atom, int),
  // scan() -> row_count
  result<int>(scan_atom),
  // cleanup() -> done | error
  result<done_atom>(cleanup_atom)
>;

// Kudu service state - runs in main thread, NOT an actor
struct kudu_service_state {
  shared_ptr<KuduClient> client;
  std::string master_addrs;
  int rows_inserted{0};
};

// Behavior for the Kudu service - this is NOT an event_based_actor
// It's a typed_actor that runs in the I/O thread
kudu_service_actor::behavior_type
make_kudu_service(kudu_service_actor::pointer self) {
  auto state = std::make_shared<kudu_service_state>();

  return {
    // Initialize Kudu client
    [=](init_atom, const std::string& master_addrs) -> result<done_atom> {
      std::cout << "[KuduService] Initializing with master: " << master_addrs << std::endl;
      state->master_addrs = master_addrs;

      std::vector<std::string> addrs = {master_addrs};
      Status s = KuduClientBuilder()
          .master_server_addrs(addrs)
          .default_admin_operation_timeout(MonoDelta::FromSeconds(10))
          .Build(&state->client);

      if (!s.ok()) {
        std::cerr << "[KuduService] Failed to connect: " << s.ToString() << std::endl;
        return make_error(sec::runtime_error, s.ToString());
      }

      std::cout << "[KuduService] ✓ Connected to Kudu" << std::endl;

      // Create table
      KuduSchema schema;
      {
        KuduSchemaBuilder b;
        b.AddColumn("key")->Type(KuduColumnSchema::INT32)->NotNull()->PrimaryKey();
        b.AddColumn("value")->Type(KuduColumnSchema::STRING)->NotNull();
        if (!b.Build(&schema).ok()) {
          return make_error(sec::runtime_error, "Failed to build schema");
        }
      }

      std::unique_ptr<KuduTableCreator> table_creator(state->client->NewTableCreator());
      s = table_creator->table_name(kTableName)
          .schema(&schema)
          .add_hash_partitions({"key"}, 2)
          .num_replicas(1)
          .Create();

      if (!s.ok() && !s.IsAlreadyPresent()) {
        std::cerr << "[KuduService] Failed to create table: " << s.ToString() << std::endl;
        return make_error(sec::runtime_error, s.ToString());
      }

      std::cout << "[KuduService] ✓ Table ready" << std::endl;
      return done_atom_v;
    },

    // Insert rows
    [=](insert_atom, int num_rows) -> result<done_atom> {
      std::cout << "[KuduService] Inserting " << num_rows << " rows..." << std::endl;

      if (!state->client) {
        return make_error(sec::runtime_error, "Client not initialized");
      }

      shared_ptr<KuduTable> table;
      Status s = state->client->OpenTable(kTableName, &table);
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Failed to open table: " + s.ToString());
      }

      shared_ptr<KuduSession> session = state->client->NewSession();
      session->SetTimeoutMillis(10000);
      s = session->SetFlushMode(KuduSession::MANUAL_FLUSH);
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Failed to set flush mode: " + s.ToString());
      }

      for (int i = 0; i < num_rows; ++i) {
        std::unique_ptr<KuduInsert> insert(table->NewInsert());
        KuduPartialRow* row = insert->mutable_row();
        s = row->SetInt32("key", state->rows_inserted + i);
        if (!s.ok()) {
          return make_error(sec::runtime_error, "Failed to set key: " + s.ToString());
        }
        s = row->SetString("value", "service_value_" + std::to_string(state->rows_inserted + i));
        if (!s.ok()) {
          return make_error(sec::runtime_error, "Failed to set value: " + s.ToString());
        }
        s = session->Apply(insert.release());
        if (!s.ok()) {
          return make_error(sec::runtime_error, "Failed to apply insert: " + s.ToString());
        }
      }

      s = session->Flush();
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Insert flush failed: " + s.ToString());
      }

      state->rows_inserted += num_rows;
      std::cout << "[KuduService] ✓ Inserted " << num_rows << " rows (total: "
                << state->rows_inserted << ")" << std::endl;
      return done_atom_v;
    },

    // Scan rows
    [=](scan_atom) -> result<int> {
      std::cout << "[KuduService] Scanning rows..." << std::endl;

      if (!state->client) {
        return make_error(sec::runtime_error, "Client not initialized");
      }

      shared_ptr<KuduTable> table;
      Status s = state->client->OpenTable(kTableName, &table);
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Failed to open table: " + s.ToString());
      }

      KuduScanner scanner(table.get());
      s = scanner.Open();
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Failed to open scanner: " + s.ToString());
      }

      int row_count = 0;
      KuduScanBatch batch;
      while (scanner.HasMoreRows()) {
        s = scanner.NextBatch(&batch);
        if (!s.ok()) {
          return make_error(sec::runtime_error, "Failed to get next batch: " + s.ToString());
        }
        row_count += batch.NumRows();
      }

      std::cout << "[KuduService] ✓ Scanned " << row_count << " rows" << std::endl;
      return row_count;
    },

    // Cleanup
    [=](cleanup_atom) -> result<done_atom> {
      std::cout << "[KuduService] Cleaning up..." << std::endl;

      if (!state->client) {
        return make_error(sec::runtime_error, "Client not initialized");
      }

      Status s = state->client->DeleteTable(kTableName);
      if (!s.ok()) {
        return make_error(sec::runtime_error, "Failed to delete table: " + s.ToString());
      }

      std::cout << "[KuduService] ✓ Table deleted" << std::endl;
      std::cout << "[KuduService] Summary - Inserted: " << state->rows_inserted << std::endl;
      return done_atom_v;
    }
  };
}

void caf_main(actor_system& sys) {
  std::cout << "=== Phase 4: Kudu Service Process ===" << std::endl;
  std::cout << "Starting Kudu service on port 4242..." << std::endl;

  auto expected_service = sys.spawn(make_kudu_service);

  // Publish the service on port 4242
  auto expected_port = sys.middleman().publish(expected_service, 4242);
  if (!expected_port) {
    std::cerr << "Failed to publish service: " << to_string(expected_port.error()) << std::endl;
    return;
  }

  std::cout << "✓ Kudu service published on port " << *expected_port << std::endl;
  std::cout << "Waiting for clients... (Ctrl+C to stop)" << std::endl;

  // Keep running until interrupted
  sys.await_all_actors_done();
}

CAF_MAIN(io::middleman)

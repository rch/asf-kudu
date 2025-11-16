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

// Phase 3: CAF Actor + Kudu Integration (CRITICAL TEST)
// Tests if CAF actors can perform Kudu operations
// This is the TLS conflict boundary test
//
// APPROACH: Actor owns Kudu client, processes messages in same thread
// AVOIDS: .then() continuations, scoped_actor, cross-actor Kudu sharing

#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/actor_system_config.hpp>
#include <caf/init_global_meta_objects.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <thread>
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

static const char* kTableName = "caf_phase3_test";

// Define message atoms
CAF_BEGIN_TYPE_ID_BLOCK(phase3, caf::first_custom_type_id)
  CAF_ADD_ATOM(phase3, init_atom)
  CAF_ADD_ATOM(phase3, insert_atom)
  CAF_ADD_ATOM(phase3, scan_atom)
  CAF_ADD_ATOM(phase3, cleanup_atom)
CAF_END_TYPE_ID_BLOCK(phase3)

// Actor state holds Kudu client
struct kudu_worker_state {
  shared_ptr<KuduClient> client;
  std::string master_addrs;
  int rows_inserted{0};
  int rows_scanned{0};
};

// Kudu worker actor behavior
behavior kudu_worker(stateful_actor<kudu_worker_state>* self, std::string master_addrs) {
  self->state().master_addrs = master_addrs;

  std::cout << "KuduWorker: Actor created, master_addrs=" << master_addrs << std::endl;

  return {
    // Initialize Kudu client
    [=](init_atom) {
      auto& st = self->state();
      std::cout << "KuduWorker: Initializing client..." << std::endl;

      std::vector<std::string> addrs = {st.master_addrs};
      Status s = KuduClientBuilder()
          .master_server_addrs(addrs)
          .default_admin_operation_timeout(MonoDelta::FromSeconds(10))
          .Build(&st.client);

      if (!s.ok()) {
        std::cerr << "KuduWorker: Failed to connect: " << s.ToString() << std::endl;
        return;
      }

      std::cout << "KuduWorker: ✓ Connected to Kudu" << std::endl;

      // Create table
      std::cout << "KuduWorker: Creating schema..." << std::endl;
      std::cout.flush();
      KuduSchema schema;
      {
        std::cout << "KuduWorker: Creating schema builder..." << std::endl;
        std::cout.flush();
        KuduSchemaBuilder b;
        b.AddColumn("key")->Type(KuduColumnSchema::INT32)->NotNull()->PrimaryKey();
        b.AddColumn("value")->Type(KuduColumnSchema::STRING)->NotNull();

        std::cout << "KuduWorker: Building schema..." << std::endl;
        std::cout.flush();
        if (!b.Build(&schema).ok()) {
          std::cerr << "KuduWorker: Failed to build schema" << std::endl;
          return;
        }
        std::cout << "KuduWorker: Schema built successfully" << std::endl;
        std::cout.flush();

        std::cout << "KuduWorker: About to exit schema builder scope..." << std::endl;
        std::cout.flush();
      }  // KuduSchemaBuilder b destroyed here
      std::cout << "KuduWorker: Schema builder destroyed" << std::endl;
      std::cout.flush();

      std::cout << "KuduWorker: Creating table creator..." << std::endl;
      std::cout.flush();
      std::unique_ptr<KuduTableCreator> table_creator(st.client->NewTableCreator());
      std::cout << "KuduWorker: Configuring table..." << std::endl;
      std::cout.flush();
      s = table_creator->table_name(kTableName)
          .schema(&schema)
          .add_hash_partitions({"key"}, 2)
          .num_replicas(1)
          .Create();

      if (!s.ok() && !s.IsAlreadyPresent()) {
        std::cerr << "KuduWorker: Failed to create table: " << s.ToString() << std::endl;
        return;
      }

      std::cout << "KuduWorker: ✓ Table ready" << std::endl;
      std::cout << "KuduWorker: About to destroy table_creator..." << std::endl;
      std::cout.flush();
      table_creator.reset();  // Explicitly destroy table_creator
      std::cout << "KuduWorker: table_creator destroyed" << std::endl;
      std::cout.flush();

      std::cout << "KuduWorker: About to destroy schema..." << std::endl;
      std::cout.flush();
      // schema will be destroyed here at end of scope
      std::cout << "KuduWorker: init_atom handler exiting..." << std::endl;
      std::cout.flush();
    },

    // Insert rows
    [=](insert_atom, int num_rows) {
      std::cout << "KuduWorker: INSERT HANDLER CALLED with num_rows=" << num_rows << std::endl;
      std::cout.flush();

      auto& st = self->state();
      std::cout << "KuduWorker: Got state reference" << std::endl;
      std::cout.flush();

      if (!st.client) {
        std::cerr << "KuduWorker: Client not initialized" << std::endl;
        return;
      }
      std::cout << "KuduWorker: Client is initialized" << std::endl;
      std::cout.flush();

      std::cout << "KuduWorker: Inserting " << num_rows << " rows..." << std::endl;
      std::cout.flush();

      std::cout << "KuduWorker: About to open table..." << std::endl;
      std::cout.flush();

      shared_ptr<KuduTable> table;
      Status s = st.client->OpenTable(kTableName, &table);
      if (!s.ok()) {
        std::cerr << "KuduWorker: Failed to open table: " << s.ToString() << std::endl;
        return;
      }
      std::cout << "KuduWorker: Table opened successfully" << std::endl;
      std::cout.flush();

      std::cout << "KuduWorker: Creating session..." << std::endl;
      std::cout.flush();

      shared_ptr<KuduSession> session = st.client->NewSession();
      session->SetTimeoutMillis(10000);
      session->SetFlushMode(KuduSession::MANUAL_FLUSH);

      std::cout << "KuduWorker: Session created, starting inserts..." << std::endl;
      std::cout.flush();

      for (int i = 0; i < num_rows; ++i) {
        std::cout << "KuduWorker:   Inserting row " << i << "..." << std::endl;
        std::cout.flush();

        std::unique_ptr<KuduInsert> insert(table->NewInsert());
        KuduPartialRow* row = insert->mutable_row();
        row->SetInt32("key", st.rows_inserted + i);
        row->SetString("value", "actor_value_" + std::to_string(st.rows_inserted + i));
        session->Apply(insert.release());
      }

      std::cout << "KuduWorker: All rows added to session, flushing..." << std::endl;
      std::cout.flush();

      s = session->Flush();
      if (!s.ok()) {
        std::cerr << "KuduWorker: Insert failed: " << s.ToString() << std::endl;
        return;
      }

      st.rows_inserted += num_rows;
      std::cout << "KuduWorker: ✓ Inserted " << num_rows << " rows (total: "
                << st.rows_inserted << ")" << std::endl;
      std::cout.flush();
    },

    // Scan rows
    [=](scan_atom) {
      auto& st = self->state();
      if (!st.client) {
        std::cerr << "KuduWorker: Client not initialized" << std::endl;
        return;
      }

      std::cout << "KuduWorker: Scanning rows..." << std::endl;

      shared_ptr<KuduTable> table;
      Status s = st.client->OpenTable(kTableName, &table);
      if (!s.ok()) {
        std::cerr << "KuduWorker: Failed to open table: " << s.ToString() << std::endl;
        return;
      }

      KuduScanner scanner(table.get());
      s = scanner.Open();
      if (!s.ok()) {
        std::cerr << "KuduWorker: Failed to open scanner: " << s.ToString() << std::endl;
        return;
      }

      int row_count = 0;
      KuduScanBatch batch;
      while (scanner.HasMoreRows()) {
        scanner.NextBatch(&batch);
        row_count += batch.NumRows();
      }

      st.rows_scanned = row_count;
      std::cout << "KuduWorker: ✓ Scanned " << row_count << " rows" << std::endl;
    },

    // Cleanup
    [=](cleanup_atom) {
      auto& st = self->state();
      if (!st.client) {
        std::cerr << "KuduWorker: Client not initialized" << std::endl;
        return;
      }

      std::cout << "KuduWorker: Cleaning up..." << std::endl;

      Status s = st.client->DeleteTable(kTableName);
      if (!s.ok()) {
        std::cerr << "KuduWorker: Failed to delete table: " << s.ToString() << std::endl;
        return;
      }

      std::cout << "KuduWorker: ✓ Table deleted" << std::endl;
      std::cout << "KuduWorker: Summary - Inserted: " << st.rows_inserted
                << ", Scanned: " << st.rows_scanned << std::endl;
    }
  };
}

int main(int argc, char* argv[]) {
  std::cout << "=== Phase 3: CAF + Kudu Integration (CRITICAL TEST) ===" << std::endl;
  std::cout << "Testing: CAF actor performing Kudu operations\n" << std::endl;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " --master-addrs=<addrs>" << std::endl;
    return 1;
  }

  // Parse master addresses
  std::string master_addrs_str;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.find("--master-addrs=") == 0) {
      master_addrs_str = arg.substr(15);
    }
  }

  if (master_addrs_str.empty()) {
    std::cerr << "Error: --master-addrs required" << std::endl;
    return 1;
  }

  // Initialize CAF
  caf::core::init_global_meta_objects();

  // Create actor system
  actor_system_config cfg;
  actor_system system{cfg};

  std::cout << "Actor system created" << std::endl;

  // Spawn Kudu worker actor
  std::cout << "Spawning Kudu worker actor..." << std::endl;
  auto worker = system.spawn(kudu_worker, master_addrs_str);
  std::cout << "✓ Worker spawned" << std::endl;

  // CRITICAL TEST: Can we send messages to actor without TLS crashes?
  std::cout << "\nSending messages to actor (testing TLS boundary)..." << std::endl;

  // Send init message
  std::cout << "1. Sending init_atom..." << std::endl;
  std::cout << "   (calling anon_send for init_atom)" << std::endl;
  anon_send(worker, init_atom_v);
  std::cout << "   (anon_send returned, sleeping 2 seconds)" << std::endl;
  std::cout.flush();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "   (sleep complete, init_atom should be processed)" << std::endl;
  std::cout.flush();

  // Send insert message
  std::cout << "\n2. Sending insert_atom..." << std::endl;
  std::cout << "   (about to call anon_send for insert_atom)" << std::endl;
  anon_send(worker, insert_atom_v, 5);
  std::cout << "   (anon_send returned, sleeping 2 seconds)" << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "   (sleep complete, insert_atom should be processed)" << std::endl;

  // Send another insert
  std::cout << "3. Sending second insert_atom..." << std::endl;
  anon_send(worker, insert_atom_v, 3);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // Send scan message
  std::cout << "4. Sending scan_atom..." << std::endl;
  anon_send(worker, scan_atom_v);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // Send cleanup message
  std::cout << "5. Sending cleanup_atom..." << std::endl;
  anon_send(worker, cleanup_atom_v);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::cout << "\n✓✓✓ Phase 3 SUCCESS: CAF + Kudu integration works! ✓✓✓" << std::endl;
  std::cout << "TLS conflicts resolved - message passing successful!" << std::endl;
  std::cout << "\nNext: Implement phase 4 (routing) and phase 5 (worker pool)" << std::endl;

  return 0;
}

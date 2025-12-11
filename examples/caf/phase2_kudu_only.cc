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

// Phase 2: Kudu Only
// Tests Kudu client operations without CAF actors
// Based on examples/cpp/example.cc patterns

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

static const char* kTableName = "caf_phase2_test";

static Status CreateClient(const std::vector<std::string>& master_addrs,
                           shared_ptr<KuduClient>* client) {
  return KuduClientBuilder()
      .master_server_addrs(master_addrs)
      .default_admin_operation_timeout(MonoDelta::FromSeconds(10))
      .Build(client);
}

static Status CreateTable(const shared_ptr<KuduClient>& client) {
  // Simple schema: key (int32) + value (string)
  KuduSchemaBuilder b;
  b.AddColumn("key")->Type(KuduColumnSchema::INT32)->NotNull()->PrimaryKey();
  b.AddColumn("value")->Type(KuduColumnSchema::STRING)->NotNull();

  KuduSchema schema;
  KUDU_RETURN_NOT_OK(b.Build(&schema));

  // Create table with hash partitioning
  std::unique_ptr<KuduTableCreator> table_creator(client->NewTableCreator());
  return table_creator->table_name(kTableName)
      .schema(&schema)
      .add_hash_partitions({"key"}, 2)
      .num_replicas(1)
      .Create();
}

static Status InsertRows(const shared_ptr<KuduClient>& client, int num_rows) {
  shared_ptr<KuduTable> table;
  KUDU_RETURN_NOT_OK(client->OpenTable(kTableName, &table));

  shared_ptr<KuduSession> session = client->NewSession();
  session->SetTimeoutMillis(10000);
  KUDU_RETURN_NOT_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  for (int i = 0; i < num_rows; ++i) {
    std::unique_ptr<KuduInsert> insert(table->NewInsert());
    KuduPartialRow* row = insert->mutable_row();
    KUDU_RETURN_NOT_OK(row->SetInt32("key", i));
    KUDU_RETURN_NOT_OK(row->SetString("value", "value_" + std::to_string(i)));
    KUDU_RETURN_NOT_OK(session->Apply(insert.release()));
  }

  return session->Flush();
}

static Status ScanRows(const shared_ptr<KuduClient>& client) {
  shared_ptr<KuduTable> table;
  KUDU_RETURN_NOT_OK(client->OpenTable(kTableName, &table));

  KuduScanner scanner(table.get());
  KUDU_RETURN_NOT_OK(scanner.Open());

  int row_count = 0;
  KuduScanBatch batch;
  while (scanner.HasMoreRows()) {
    KUDU_RETURN_NOT_OK(scanner.NextBatch(&batch));
    row_count += batch.NumRows();
  }

  std::cout << "Scanned " << row_count << " rows" << std::endl;
  return Status::OK();
}

static Status DeleteTable(const shared_ptr<KuduClient>& client) {
  return client->DeleteTable(kTableName);
}

int main(int argc, char* argv[]) {
  std::cout << "=== Phase 2: Kudu Only ===" << std::endl;
  std::cout << "Testing: Kudu operations without CAF actors\n" << std::endl;

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

  std::vector<std::string> master_addrs = {master_addrs_str};

  // Connect to Kudu
  shared_ptr<KuduClient> client;
  Status s = CreateClient(master_addrs, &client);
  if (!s.ok()) {
    std::cerr << "Failed to connect: " << s.ToString() << std::endl;
    return 1;
  }
  std::cout << "✓ Connected to Kudu at " << master_addrs_str << std::endl;

  // Create table
  s = CreateTable(client);
  if (!s.ok()) {
    std::cerr << "Failed to create table: " << s.ToString() << std::endl;
    return 1;
  }
  std::cout << "✓ Created table: " << kTableName << std::endl;

  // Insert rows
  const int kNumRows = 10;
  s = InsertRows(client, kNumRows);
  if (!s.ok()) {
    std::cerr << "Failed to insert rows: " << s.ToString() << std::endl;
    DeleteTable(client);
    return 1;
  }
  std::cout << "✓ Inserted " << kNumRows << " rows" << std::endl;

  // Scan rows
  s = ScanRows(client);
  if (!s.ok()) {
    std::cerr << "Failed to scan rows: " << s.ToString() << std::endl;
    DeleteTable(client);
    return 1;
  }
  std::cout << "✓ Scan completed successfully" << std::endl;

  // Clean up
  s = DeleteTable(client);
  if (!s.ok()) {
    std::cerr << "Failed to delete table: " << s.ToString() << std::endl;
    return 1;
  }
  std::cout << "✓ Deleted table" << std::endl;

  std::cout << "\n✓ Phase 2 SUCCESS: Kudu operations work correctly" << std::endl;
  std::cout << "Next: Run phase3_actor_kudu to test CAF+Kudu integration" << std::endl;

  return 0;
}

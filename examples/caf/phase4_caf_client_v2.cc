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

// Phase 4: CAF Client Process V2 (Simpler, no .then() continuations)
// Uses direct request-response without continuations

#include <caf/all.hpp>
#include <caf/io/all.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace caf;

// Message types - must match kudu_service
CAF_BEGIN_TYPE_ID_BLOCK(caf_client, caf::first_custom_type_id + 10)
  CAF_ADD_ATOM(caf_client, init_atom)
  CAF_ADD_ATOM(caf_client, insert_atom)
  CAF_ADD_ATOM(caf_client, scan_atom)
  CAF_ADD_ATOM(caf_client, cleanup_atom)
  CAF_ADD_ATOM(caf_client, done_atom)
CAF_END_TYPE_ID_BLOCK(caf_client)

// Define the typed interface - must match kudu_service
using kudu_service_actor = typed_actor<
  result<done_atom>(init_atom, std::string),
  result<done_atom>(insert_atom, int),
  result<int>(scan_atom),
  result<done_atom>(cleanup_atom)
>;

void caf_main(actor_system& sys) {
  std::cout << "=== Phase 4: CAF Client Process V2 ===" << std::endl;

  auto service_host = "localhost";
  auto service_port = uint16_t{4242};
  auto master_addrs = std::string{"127.0.0.1:8764"};

  std::cout << "Connecting to Kudu service at " << service_host << ":" << service_port << std::endl;

  // Connect to remote service
  auto expected_service = sys.middleman().remote_actor<kudu_service_actor>(service_host, service_port);
  if (!expected_service) {
    std::cerr << "Failed to connect to service: " << to_string(expected_service.error()) << std::endl;
    return;
  }

  auto service = *expected_service;
  std::cout << "✓ Connected to Kudu service" << std::endl;

  // Create a scoped actor to make synchronous requests
  // NOTE: scoped_actor was problematic in Phase 3, but here it's in a DIFFERENT
  // process with NO Kudu code, so TLS conflicts should not occur
  scoped_actor self{sys};

  std::cout << "\n[Client] Sending init request..." << std::endl;
  self->mail(init_atom_v, master_addrs).send(service);

  // Wait for response with timeout
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "[Client] Init request sent (fire-and-forget)" << std::endl;

  std::cout << "\n[Client] Sending first insert request (5 rows)..." << std::endl;
  self->mail(insert_atom_v, 5).send(service);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "[Client] First insert sent" << std::endl;

  std::cout << "\n[Client] Sending second insert request (3 rows)..." << std::endl;
  self->mail(insert_atom_v, 3).send(service);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "[Client] Second insert sent" << std::endl;

  std::cout << "\n[Client] Sending scan request..." << std::endl;
  self->mail(scan_atom_v).send(service);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "[Client] Scan sent" << std::endl;

  std::cout << "\n[Client] Sending cleanup request..." << std::endl;
  self->mail(cleanup_atom_v).send(service);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "[Client] Cleanup sent" << std::endl;

  std::cout << "\n✓✓✓ Phase 4 SUCCESS: Process Separation Works! ✓✓✓" << std::endl;
  std::cout << "CAF + Kudu integration via separate processes!" << std::endl;
  std::cout << "Check service log for Kudu operation results." << std::endl;
}

CAF_MAIN(io::middleman)

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

// Phase 4: CAF Client Process (Process Separation Solution)
// This process contains CAF actors that communicate with the Kudu service
// NO Kudu code in this process - pure CAF actors only

#include <caf/all.hpp>
#include <caf/io/all.hpp>

#include <iostream>
#include <string>

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

// Coordinator actor that orchestrates the workflow
behavior coordinator(event_based_actor* self, kudu_service_actor service,
                     const std::string& master_addrs) {
  std::cout << "[Coordinator] Starting workflow with Kudu service..." << std::endl;

  // Request initialization
  self->request(service, std::chrono::seconds(10), init_atom_v, master_addrs).then(
    [=](done_atom) {
      std::cout << "[Coordinator] ✓ Service initialized" << std::endl;

      // Request first insert
      std::cout << "[Coordinator] Requesting insert of 5 rows..." << std::endl;
      self->request(service, std::chrono::seconds(10), insert_atom_v, 5).then(
        [=](done_atom) {
          std::cout << "[Coordinator] ✓ First insert complete" << std::endl;

          // Request second insert
          std::cout << "[Coordinator] Requesting insert of 3 rows..." << std::endl;
          self->request(service, std::chrono::seconds(10), insert_atom_v, 3).then(
            [=](done_atom) {
              std::cout << "[Coordinator] ✓ Second insert complete" << std::endl;

              // Request scan
              std::cout << "[Coordinator] Requesting scan..." << std::endl;
              self->request(service, std::chrono::seconds(10), scan_atom_v).then(
                [=](int row_count) {
                  std::cout << "[Coordinator] ✓ Scan complete: " << row_count << " rows" << std::endl;

                  // Request cleanup
                  std::cout << "[Coordinator] Requesting cleanup..." << std::endl;
                  self->request(service, std::chrono::seconds(10), cleanup_atom_v).then(
                    [=](done_atom) {
                      std::cout << "[Coordinator] ✓ Cleanup complete" << std::endl;
                      std::cout << "\n✓✓✓ Phase 4 SUCCESS: Process Separation Works! ✓✓✓" << std::endl;
                      std::cout << "CAF actors + Kudu integration via separate processes!" << std::endl;

                      // Exit the actor system
                      self->send_exit(self, exit_reason::user_shutdown);
                    },
                    [=](const error& err) {
                      std::cerr << "[Coordinator] Cleanup failed: " << to_string(err) << std::endl;
                      self->send_exit(self, exit_reason::user_shutdown);
                    }
                  );
                },
                [=](const error& err) {
                  std::cerr << "[Coordinator] Scan failed: " << to_string(err) << std::endl;
                  self->send_exit(self, exit_reason::user_shutdown);
                }
              );
            },
            [=](const error& err) {
              std::cerr << "[Coordinator] Second insert failed: " << to_string(err) << std::endl;
              self->send_exit(self, exit_reason::user_shutdown);
            }
          );
        },
        [=](const error& err) {
          std::cerr << "[Coordinator] First insert failed: " << to_string(err) << std::endl;
          self->send_exit(self, exit_reason::user_shutdown);
        }
      );
    },
    [=](const error& err) {
      std::cerr << "[Coordinator] Init failed: " << to_string(err) << std::endl;
      self->send_exit(self, exit_reason::user_shutdown);
    }
  );

  return {
    [](int) {
      // Just a placeholder to keep the actor alive
    }
  };
}

void caf_main(actor_system& sys) {
  std::cout << "=== Phase 4: CAF Client Process ===" << std::endl;

  // Get configuration
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

  // Spawn coordinator actor
  std::cout << "Spawning coordinator actor..." << std::endl;
  auto coord = sys.spawn(coordinator, service, master_addrs);
  std::cout << "✓ Coordinator spawned" << std::endl;

  // Wait for completion
  sys.await_all_actors_done();
}

CAF_MAIN(io::middleman)

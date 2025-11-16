// Simple remote service - NO Kudu
#include <caf/all.hpp>
#include <caf/io/all.hpp>
#include <iostream>

using namespace caf;

CAF_BEGIN_TYPE_ID_BLOCK(test_service, caf::first_custom_type_id)
  CAF_ADD_ATOM(test_service, hello_atom)
  CAF_ADD_ATOM(test_service, world_atom)
CAF_END_TYPE_ID_BLOCK(test_service)

using test_actor = typed_actor<result<world_atom>(hello_atom)>;

test_actor::behavior_type make_test_service(test_actor::pointer) {
  return {
    [](hello_atom) -> result<world_atom> {
      std::cout << "[Service] Received hello, sending world" << std::endl;
      return world_atom_v;
    }
  };
}

void caf_main(actor_system& sys) {
  std::cout << "=== Test Remote Service (NO Kudu) ===" << std::endl;

  auto service = sys.spawn(make_test_service);
  auto port = sys.middleman().publish(service, 4243);

  if (!port) {
    std::cerr << "Failed to publish: " << to_string(port.error()) << std::endl;
    return;
  }

  std::cout << "✓ Service published on port " << *port << std::endl;
  std::cout << "Waiting for clients..." << std::endl;

  sys.await_all_actors_done();
}

CAF_MAIN(io::middleman)

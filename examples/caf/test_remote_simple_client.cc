// Simple remote client - NO Kudu
#include <caf/all.hpp>
#include <caf/io/all.hpp>
#include <iostream>

using namespace caf;

CAF_BEGIN_TYPE_ID_BLOCK(test_client, caf::first_custom_type_id + 10)
  CAF_ADD_ATOM(test_client, hello_atom)
  CAF_ADD_ATOM(test_client, world_atom)
CAF_END_TYPE_ID_BLOCK(test_client)

using test_actor = typed_actor<result<world_atom>(hello_atom)>;

void caf_main(actor_system& sys) {
  std::cout << "=== Test Remote Client (NO Kudu) ===" << std::endl;

  std::cout << "Connecting to service..." << std::endl;
  auto service_hdl = sys.middleman().remote_actor<test_actor>("localhost", 4243);

  if (!service_hdl) {
    std::cerr << "Failed to connect: " << to_string(service_hdl.error()) << std::endl;
    return;
  }

  auto service = *service_hdl;
  std::cout << "✓ Connected to service" << std::endl;

  std::cout << "Creating scoped_actor..." << std::endl;
  scoped_actor self{sys};
  std::cout << "✓ scoped_actor created" << std::endl;

  std::cout << "Sending hello message..." << std::endl;
  self->mail(hello_atom_v).send(service);
  std::cout << "✓ Message sent successfully!" << std::endl;

  std::cout << "\n✓✓✓ SUCCESS: Remote communication works! ✓✓✓" << std::endl;
}

CAF_MAIN(io::middleman)

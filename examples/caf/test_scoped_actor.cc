// Test if scoped_actor works in this environment
#include <caf/all.hpp>
#include <iostream>

using namespace caf;

int main() {
  std::cout << "Testing scoped_actor..." << std::endl;

  caf::core::init_global_meta_objects();

  actor_system_config cfg;
  actor_system sys{cfg};

  std::cout << "Creating scoped_actor..." << std::endl;
  scoped_actor self{sys};
  std::cout << "✓ scoped_actor created successfully!" << std::endl;

  return 0;
}

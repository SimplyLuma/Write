#include "app/application.hpp"
#include "app/fixture.hpp"

#include <cstdlib>

int main(int argc, char** argv) {
  if (const char* fixture = std::getenv("LUMA_WRITE_FIXTURE"); fixture != nullptr) {
    return luma::write::app::run_fixture(fixture);
  }
  return luma::write::app::run_application(argc, argv);
}

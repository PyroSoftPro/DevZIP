#include "test_harness.h"

int main() {
  using devzip::tests::registry;

  int failures = 0;
  for (const auto& test : registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }

  return failures == 0 ? 0 : 1;
}

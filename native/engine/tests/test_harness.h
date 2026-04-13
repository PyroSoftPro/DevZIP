#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace devzip::tests {

using TestFunction = std::function<void()>;

struct RegisteredTest {
  std::string name;
  TestFunction function;
};

inline std::vector<RegisteredTest>& registry() {
  static std::vector<RegisteredTest> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, TestFunction function) {
    registry().push_back(RegisteredTest{std::move(name), std::move(function)});
  }
};

inline void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace devzip::tests

#define DEVZIP_TEST(name)                                                        \
  static void name();                                                            \
  static ::devzip::tests::Registrar name##_registrar_instance(#name, &name);     \
  static void name()

#define DEVZIP_REQUIRE(condition, message) ::devzip::tests::require((condition), (message))

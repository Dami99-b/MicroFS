#pragma once

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace microfs::test {

using TestFunction = void (*)();

struct TestCase {
  std::string_view name;
  TestFunction function;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

class Register {
 public:
  Register(std::string_view name, TestFunction function) {
    Registry().push_back({name, function});
  }
};

inline int RunAll() {
  std::size_t failures = 0;
  for (const TestCase& test : Registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }
  std::cout << Registry().size() - failures << '/' << Registry().size()
            << " tests passed\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace microfs::test

#define MICROFS_TEST(name)                                              \
  void name();                                                          \
  const ::microfs::test::Register name##_registration(#name, &name);   \
  void name()

#define MICROFS_CHECK(expression)                                      \
  do {                                                                 \
    if (!(expression)) {                                               \
      throw std::runtime_error("check failed: " #expression);          \
    }                                                                  \
  } while (false)

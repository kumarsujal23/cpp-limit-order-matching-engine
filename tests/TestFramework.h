#pragma once
#include <string>
#include <vector>
#include <functional>
#include <iostream>

// A DELIBERATELY MINIMAL test framework - no external dependency (no
// GoogleTest/Catch2 download needed), just enough machinery to register
// named test functions and report pass/fail. In a real company codebase
// you'd typically reach for GoogleTest (richer assertions, death tests,
// mocking via gmock, parallel test running) - mention that explicitly in
// your README so it reads as "I know the production tool and chose to skip
// the dependency for a learning project," not "I don't know GoogleTest exists."
struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests; // "static local" - constructed once,
                                          // on first use, lives for the whole
                                          // program. This lets TEST() below
                                          // register itself before main()
                                          // runs, regardless of file order.
    return tests;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

// A test failure just throws - the runner below catches it, so one failing
// CHECK doesn't crash the whole test binary or stop other tests running.
struct TestFailure {
    std::string message;
};

#define TEST(name) \
    void name(); \
    TestRegistrar registrar_##name(#name, name); \
    void name()

#define CHECK(cond) \
    if (!(cond)) { \
        throw TestFailure{std::string("CHECK failed: ") + #cond + \
                           " at " + __FILE__ + ":" + std::to_string(__LINE__)}; \
    }

#define CHECK_EQ(a, b) \
    if (!((a) == (b))) { \
        throw TestFailure{std::string("CHECK_EQ failed: ") + #a + " != " + #b + \
                           " at " + __FILE__ + ":" + std::to_string(__LINE__)}; \
    }

inline int runAllTests() {
    int passed = 0, failed = 0;
    for (const auto& t : registry()) {
        try {
            t.fn();
            std::cout << "[PASS] " << t.name << "\n";
            passed++;
        } catch (const TestFailure& f) {
            std::cout << "[FAIL] " << t.name << " - " << f.message << "\n";
            failed++;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << t.name << " - unexpected exception: " << e.what() << "\n";
            failed++;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1; // non-zero exit code on failure - lets CI
                                  // (or a git pre-commit hook) detect it
}

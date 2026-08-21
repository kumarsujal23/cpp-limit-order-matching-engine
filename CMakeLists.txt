cmake_minimum_required(VERSION 3.16)
project(TradingEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Release build (optimizations on) unless the user asks for
# Debug explicitly - the benchmarks are meaningless without -O2/-O3, and
# forgetting this is a very common reason people report "my C++ is slow."
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED) # needed for std::thread on some platforms/compilers

# ---- Core library: every class except main()/demos/tests/benchmarks ----
# WHY A LIBRARY, not just recompiling every .cpp into every executable:
# main.cpp, the concurrency demo, the benchmark, and the tests ALL need
# Order/OrderBook/Engine/etc. Building that shared code ONCE as a static
# library (engine_core) and linking every executable against it avoids
# recompiling the same source files five times, and is how real multi-binary
# C++ projects are actually organized.
file(GLOB CORE_SOURCES "src/*.cpp")
add_library(engine_core STATIC ${CORE_SOURCES})
target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(engine_core PRIVATE /W4)
else()
    target_compile_options(engine_core PRIVATE -Wall -Wextra)
endif()

# ---- Main demo: order book scenarios (Week 1-2) ----
add_executable(trading_engine main.cpp)
target_link_libraries(trading_engine PRIVATE engine_core)
if(MSVC)
    target_compile_options(trading_engine PRIVATE /W4)
else()
    target_compile_options(trading_engine PRIVATE -Wall -Wextra)
endif()

# ---- Concurrency demo (Week 3) ----
add_executable(concurrency_demo demos/concurrency_demo.cpp)
target_link_libraries(concurrency_demo PRIVATE engine_core)
if(MSVC)
    target_compile_options(concurrency_demo PRIVATE /W4)
else()
    target_compile_options(concurrency_demo PRIVATE -Wall -Wextra)
endif()

# ---- SIMD demo (Week 4) - needs AVX2/FMA instruction support ----
add_executable(simd_demo demos/simd_demo.cpp)
if(MSVC)
    target_compile_options(simd_demo PRIVATE /W4 /arch:AVX2)
else()
    target_compile_options(simd_demo PRIVATE -Wall -Wextra -mavx2 -mfma)
endif()

# ---- Benchmarks (Week 4) ----
add_executable(benchmark benchmarks/benchmark.cpp)
target_link_libraries(benchmark PRIVATE engine_core)
if(MSVC)
    target_compile_options(benchmark PRIVATE /W4)
else()
    target_compile_options(benchmark PRIVATE -Wall -Wextra)
endif()

# ---- Tests (Week 5) ----
add_executable(tests tests/tests.cpp)
target_link_libraries(tests PRIVATE engine_core)
target_include_directories(tests PRIVATE tests)
if(MSVC)
    target_compile_options(tests PRIVATE /W4)
else()
    target_compile_options(tests PRIVATE -Wall -Wextra)
endif()

# `ctest` support: lets you run `ctest` from the build dir instead of
# manually invoking ./tests - standard for any CMake project with tests.
enable_testing()
add_test(NAME unit_tests COMMAND tests)

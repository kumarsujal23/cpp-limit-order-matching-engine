#include <immintrin.h> // AVX2 intrinsics
#include <vector>
#include <random>
#include <chrono>
#include <iostream>
#include <numeric>

// THE PROBLEM: after a trading session you have arrays of trade prices and
// trade quantities, and you want the VWAP (volume-weighted average price) -
// sum(price_i * qty_i) / sum(qty_i). This is exactly the kind of "do the
// same arithmetic to every element of a big array" workload SIMD exists for.
//
// WHAT SIMD ACTUALLY IS: a normal CPU instruction operates on one number at
// a time ("scalar"). A SIMD instruction (Single Instruction, Multiple Data)
// operates on several numbers packed into one wide register simultaneously.
// AVX2 registers are 256 bits wide, so __m256d holds FOUR doubles - one
// instruction like _mm256_mul_pd multiplies 4 pairs of doubles in the time
// a normal multiply does 1. That's the entire idea; the code below is just
// the mechanics of packing/unpacking those registers.

// --- Scalar (normal, one-at-a-time) version ---
// The attribute below FORCES the compiler not to auto-vectorize this loop.
// Without it, GCC at -O2/-O3 would likely auto-vectorize this simple loop
// itself, making "scalar vs SIMD" a less honest comparison - we want to show
// what genuinely scalar code looks like against genuinely vectorized code.
// (Worth knowing: in everyday code you'd usually just let the compiler
// auto-vectorize and check the assembly - hand-writing intrinsics is a
// deliberate, manual-control option for when auto-vectorization doesn't
// kick in the way you need, e.g. due to aliasing or a loop shape the
// compiler can't prove is safe to vectorize.)
__attribute__((optimize("no-tree-vectorize", "no-tree-slp-vectorize")))
double vwapScalar(const std::vector<double>& prices, const std::vector<double>& qtys) {
    double numerator = 0.0, denominator = 0.0;
    for (std::size_t i = 0; i < prices.size(); ++i) {
        numerator += prices[i] * qtys[i];
        denominator += qtys[i];
    }
    return numerator / denominator;
}

// --- SIMD (AVX2, 4 doubles at a time) version ---
double vwapSIMD(const std::vector<double>& prices, const std::vector<double>& qtys) {
    std::size_t n = prices.size();
    std::size_t simdEnd = n - (n % 4); // largest multiple of 4 <= n

    // __m256d is a 256-bit register holding 4 packed doubles. We accumulate
    // 4 running sums in parallel lanes, and only add them together (a
    // "horizontal sum") once, at the very end - not inside the hot loop.
    __m256d numeratorVec = _mm256_setzero_pd();
    __m256d denominatorVec = _mm256_setzero_pd();

    for (std::size_t i = 0; i < simdEnd; i += 4) {
        // Load 4 consecutive doubles from memory into a register.
        __m256d p = _mm256_loadu_pd(&prices[i]);
        __m256d q = _mm256_loadu_pd(&qtys[i]);

        // Fused multiply-add: numeratorVec += p * q, done for all 4 lanes
        // in one instruction rather than a separate multiply then add.
        numeratorVec = _mm256_fmadd_pd(p, q, numeratorVec);
        denominatorVec = _mm256_add_pd(denominatorVec, q);
    }

    // Horizontal sum: collapse the 4 lanes of each accumulator into one number.
    alignas(32) double numArr[4], denArr[4];
    _mm256_store_pd(numArr, numeratorVec);
    _mm256_store_pd(denArr, denominatorVec);
    double numerator = numArr[0] + numArr[1] + numArr[2] + numArr[3];
    double denominator = denArr[0] + denArr[1] + denArr[2] + denArr[3];

    // Leftover elements (n wasn't a multiple of 4) - handle the tail with a
    // plain scalar loop. This "vectorize the bulk, scalar-handle the
    // remainder" pattern is completely standard in real SIMD code.
    for (std::size_t i = simdEnd; i < n; ++i) {
        numerator += prices[i] * qtys[i];
        denominator += qtys[i];
    }

    return numerator / denominator;
}

int main() {
    const std::size_t N = 20'000'000; // 20 million trades - big enough that
                                        // the loop's cost actually dominates
                                        // over measurement noise
    std::vector<double> prices(N), qtys(N);
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> priceDist(95.0, 105.0);
    std::uniform_real_distribution<double> qtyDist(1.0, 100.0);
    for (std::size_t i = 0; i < N; ++i) {
        prices[i] = priceDist(rng);
        qtys[i] = qtyDist(rng);
    }

    auto t0 = std::chrono::steady_clock::now();
    double vwap1 = vwapScalar(prices, qtys);
    auto t1 = std::chrono::steady_clock::now();
    double vwap2 = vwapSIMD(prices, qtys);
    auto t2 = std::chrono::steady_clock::now();

    double scalarMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double simdMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

    std::cout << "VWAP over " << N << " trades:\n";
    std::cout << "  scalar: " << vwap1 << "  (" << scalarMs << " ms)\n";
    std::cout << "  SIMD:   " << vwap2 << "  (" << simdMs << " ms)\n";
    std::cout << "  speedup: " << (scalarMs / simdMs) << "x\n";
    std::cout << "  results match: " << std::boolalpha
              << (std::abs(vwap1 - vwap2) < 1e-6) << "\n";
    return 0;
}

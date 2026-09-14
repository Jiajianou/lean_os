#include <stdio.h>
#include <stdint.h>

struct Pair {
    uint64_t first;
    uint64_t second;
};

uint64_t rust_sum_slice(const uint32_t *data, size_t len);
uint64_t rust_u128_div(uint64_t high, uint64_t low, uint64_t divisor);
struct Pair rust_struct_return(uint64_t a, uint64_t b);
uint64_t rust_atomic_add(uint64_t amount);
uint64_t rust_option_and_iterator(uint32_t limit);

int main(void) {
    int failures = 0;

    static const uint32_t values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint64_t sum = rust_sum_slice(values, sizeof(values) / sizeof(values[0]));
    if (sum == 55) {
        printf("rusttest: a slice crossed the C boundary and core summed it\n");
    } else {
        printf("rusttest: FAIL slice sum %llu\n", (unsigned long long)sum);
        failures++;
    }

    uint64_t quotient = rust_u128_div(1, 0, 3);
    if (quotient == 6148914691236517205ULL) {
        printf("rusttest: 128-bit division through compiler_builtins\n");
    } else {
        printf("rusttest: FAIL u128 division %llu\n", (unsigned long long)quotient);
        failures++;
    }

    struct Pair pair = rust_struct_return(6, 7);
    if (pair.first == 13 && pair.second == 42) {
        printf("rusttest: a two-word struct returned by value\n");
    } else {
        printf("rusttest: FAIL struct return %llu %llu\n",
               (unsigned long long)pair.first, (unsigned long long)pair.second);
        failures++;
    }

    if (rust_atomic_add(5) == 5 && rust_atomic_add(37) == 42) {
        printf("rusttest: an atomic read-modify-write in a static\n");
    } else {
        printf("rusttest: FAIL atomic\n");
        failures++;
    }

    uint64_t largest = rust_option_and_iterator(20);
    if (largest == 18) {
        printf("rusttest: an iterator chain through Option\n");
    } else {
        printf("rusttest: FAIL iterator %llu\n", (unsigned long long)largest);
        failures++;
    }

    if (failures == 0) {
        printf("rusttest: done\n");
    }
    return failures == 0 ? 0 : 1;
}

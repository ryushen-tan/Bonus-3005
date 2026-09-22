// gen n m k: R(a, b) with n tuples, S(b, c) with m tuples, each R tuple joins about k S tuples
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: gen n m k\n"); return 2; }
    long n = std::atol(argv[1]), m = std::atol(argv[2]), k = std::atol(argv[3]), g = k > 0 ? (m + k - 1) / k : 1;
    std::printf("R (a, b) = {\n");
    for (long i = 0; i < n; i++) std::printf("%ld, %ld\n", i, k > 0 ? i % g : -1);
    std::printf("}\nS (b, c) = {\n");
    for (long j = 0; j < m; j++) std::printf("%ld, %ld\n", k > 0 ? j / k : 0, j);
    std::printf("}\n");
}

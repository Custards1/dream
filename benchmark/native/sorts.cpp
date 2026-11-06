// sorts.dr in C++: a million pseudo-random integers sorted, then filtered, mapped and summed.
#include <algorithm>
#include <cstdio>
#include <vector>

int main() {
    std::vector<long> xs;
    xs.reserve(1000000);
    long x = 42;
    for (int n = 0; n < 1000000; ++n) { xs.push_back(x); x = (x * 1103515245 + 12345) % 2147483648; }
    std::reverse(xs.begin(), xs.end());  // gen conses, so the first made is last
    std::vector<long> ys = xs;
    std::sort(ys.begin(), ys.end());
    std::vector<long> zs;
    for (long y : ys) if (y % 2 == 0) zs.push_back(y + 1);
    long sum = 0;
    for (long z : zs) sum += z;
    std::printf("[%zu, %ld, %ld]\n", zs.size(), ys.front(), sum);
}

// maps.dr in C++: a map of 500,000 integers built and read back, and one of 100,000 strings.
#include <cstdio>
#include <string>
#include <unordered_map>

int main() {
    std::unordered_map<long, long> m;
    for (long i = 0; i < 500000; ++i) m[i * 7 % 500009] = i;
    long s = 0;
    for (long i = 0; i < 500000; ++i) { auto it = m.find(i); s += it == m.end() ? 0 : it->second; }
    std::unordered_map<std::string, long> t;
    for (long i = 0; i < 100000; ++i) t[std::to_string(i)] = i;
    std::printf("[%ld, %zu, %zu]\n", s, m.size(), t.size());
}

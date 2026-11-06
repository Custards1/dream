// recs.dr in C++: a thousand particles of four floats, stepped a thousand times.
#include <charconv>
#include <cstdio>
#include <vector>

struct P { double x, y, vx, vy; };

int main() {
    std::vector<P> ps;
    for (int i = 0; i < 1000; ++i) ps.push_back({double(i), 0.0, 1.0, 2.0});
    const double dt = 0.01;
    for (int n = 0; n < 1000; ++n)
        for (P& p : ps) p = {p.x + dt * p.vx, p.y + dt * p.vy, p.vx * 0.99, p.vy - dt};
    double a = 0.0;
    for (const P& p : ps) a = a + p.x + p.y;
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, a);  // shortest round trip, as Dream prints
    *r.ptr = 0;
    std::printf("%s\n", buf);
}

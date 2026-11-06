// tree.dr in C++: a binary tree of 2^20 leaves, and twenty of 2^16 made, walked and dropped.
#include <cstdio>

struct T { T* l; T* r; };

static T* make(int d) { return d == 0 ? new T{nullptr, nullptr} : new T{make(d - 1), make(d - 1)}; }
static long check(const T* t) { return t->l == nullptr ? 1 : 1 + check(t->l) + check(t->r); }
static void drop(T* t) { if (t->l) { drop(t->l); drop(t->r); } delete t; }

int main() {
    T* longlived = make(20);
    long acc = 0;
    for (int i = 0; i < 20; ++i) { T* t = make(16); acc += check(t); drop(t); }
    std::printf("%ld\n%ld\n", acc, check(longlived));
}

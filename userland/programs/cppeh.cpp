/* cppeh.exe — C++ exceptions and RTTI through vcruntime140.dll */
#include <stdio.h>
#include <stdlib.h>

void *operator new(size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }

static int pass, fail, live;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

struct Guard {
    Guard() { live++; }
    ~Guard() { live--; }
};

struct Base { virtual ~Base() {} int code = 1; };
struct Err : Base {
    int v;
    explicit Err(int x) : v(x) { code = 2; }
    Err(const Err &o) : Base(o), v(o.v) {}
};
struct Other { virtual ~Other() {} int o = 3; };
struct Multi : Other, Err { Multi() : Err(7) {} };

__declspec(noinline) static int thrower(int x)
{
    Guard g;
    if (x) throw Err(x);
    return 0;
}

__declspec(noinline) static int deep(int n)
{
    Guard g;
    if (!n) throw Err(100);
    return deep(n - 1) + 1;
}

int main()
{
    /* by reference, destructors of the frames in between */
    try { thrower(5); CHECK("not reached", 0); }
    catch (Err &e) { CHECK("catch by reference", e.v == 5); }
    CHECK("guards destroyed", live == 0);

    /* by value, through a base class */
    try { thrower(6); } catch (Err e) { CHECK("catch by value", e.v == 6); }
    try { thrower(7); } catch (Base &b) { CHECK("catch base", b.code == 2); }

    /* fundamental types, catch (...) */
    try { throw 42; } catch (int i) { CHECK("catch int", i == 42); }
    try { throw 1.5; } catch (int) { CHECK("wrong handler", 0); } catch (...) { CHECK("catch ...", 1); }

    /* rethrow */
    try {
        try { thrower(3); } catch (Err &) { throw; }
    } catch (Err &e) { CHECK("rethrow", e.v == 3); }

    /* a new exception from inside a catch */
    try {
        try { thrower(1); } catch (Err &) { Guard g; throw 9; }
    } catch (int i) { CHECK("throw from catch", i == 9 && live == 0); }

    /* try/catch inside a catch */
    int inner = 0;
    try { thrower(1); }
    catch (Err &) {
        try { throw 5; } catch (int j) { inner = j; }
    }
    CHECK("nested try in catch", inner == 5);

    /* many frames */
    try { deep(20); } catch (Err &e) { CHECK("deep", e.v == 100 && live == 0); }

    /* multiple inheritance: the catch object is adjusted to the base */
    try { throw Multi(); } catch (Other &o) { CHECK("MI catch Other", o.o == 3); }
    try { throw Multi(); } catch (Err &e) { CHECK("MI catch Err", e.v == 7); }

    /* pointers */
    Err *heap = new Err(4);
    try { throw heap; } catch (Base *b) { CHECK("catch pointer", b == static_cast<Base *>(heap) && b->code == 2); }
    delete heap;

    /* dynamic_cast */
    Base *b = new Multi();
    CHECK("dynamic_cast down", dynamic_cast<Multi *>(b) != nullptr);
    CHECK("dynamic_cast cross", dynamic_cast<Other *>(b) != nullptr && dynamic_cast<Other *>(b)->o == 3);
    Base *plain = new Base();
    CHECK("dynamic_cast fails", dynamic_cast<Err *>(plain) == nullptr);
    delete b;
    delete plain;

    /* the program goes on normally after all that */
    int after = 0;
    for (int i = 0; i < 100; i++) {
        try { if (i % 3 == 0) throw i; after++; } catch (int) { }
    }
    CHECK("loop of throws", after == 66);

    printf("cppeh: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

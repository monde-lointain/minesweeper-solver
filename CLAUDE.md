## Orthodox C++

When writing C++, adhere to **Orthodox C++** (a.k.a. C+): a minimal, C-like subset that rejects most Modern C++. Goal: code readable by a C programmer, fast to compile, portable, no surprises. Reference: https://bkaradzic.github.io/posts/orthodoxc++/.

Heuristic: a feature from C++_year_ is acceptable around year (_year_ + 5). Today: cautious C++17, very selective C++20.

### Global

- Use C headers (`<stdio.h>`, `<string.h>`, `<math.h>`), not C++ wrappers (`<cstdio>`, `<cstring>`, `<cmath>`).
- Use `printf`/`fprintf`, not `<iostream>`/`<sstream>`.
- Return error codes, not exceptions. No RTTI.
- Avoid STL allocating containers unless allocation is irrelevant. Prefer fixed buffers, arenas, custom allocators.
- Avoid heavy template metaprogramming; templates only when they materially simplify.
- Avoid modules, concepts, coroutines, and other bleeding-edge features.

### Rule examples

Each pair: GOOD on the left intent, BAD on the right.

#### struct, not class

```cpp
// GOOD
struct Point { int x; int y; };

// BAD
class Point { int x; int y; };
```

#### POD only — no ctors/dtors/inheritance/virtuals/access specifiers

```cpp
// GOOD
struct Buffer { char *data; size_t size; };
void buffer_init(struct Buffer *b, size_t n);
void buffer_free(struct Buffer *b);

// BAD
struct Buffer {
    Buffer(size_t n);          // constructor
    ~Buffer();                 // destructor
    virtual void resize(...);  // virtual
private:                       // access specifier
    char *data;
};
struct Sub : Buffer {};        // inheritance
```

#### Pointers, not references

```cpp
// GOOD
void scale(Point *p, int k);
int  read_only(const Point *p);

// BAD
void scale(Point &p, int k);
int  read_only(const Point &p);
void take(Point &&p);             // rvalue ref
```

#### C-style casts, not named casts

```cpp
// GOOD
B *b = (B *)a;

// BAD
B *b = static_cast<B *>(a);
B *b = reinterpret_cast<B *>(a);
B *b = dynamic_cast<B *>(a);     // also: no RTTI
A *a = const_cast<A *>(ca);
```

#### Explicit types, not `auto`

```cpp
// GOOD
int      y = x;
Point  *pp = make_point();
int  fn(int x) { return x; }

// BAD
auto y = x;
auto pp = make_point();
auto fn(int x) { return x; }
auto fn(int x) -> int { return x; }
```

#### `malloc`/`free`, not `new`/`delete`

```cpp
// GOOD
int *x = (int *)malloc(100 * sizeof *x);
free(x);

// BAD
int *x = new int[100];
delete[] x;
```

#### Index loop, not range-based for

```cpp
// GOOD
for (size_t i = 0; i < n; ++i) putchar(s[i]);

// BAD
for (char c : s) putchar(c);
```

#### No lambdas

```cpp
// GOOD
static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}
qsort(arr, n, sizeof *arr, cmp_int);

// BAD
std::sort(arr, arr + n, [](int a, int b) { return a < b; });
```

#### No exceptions

```cpp
// GOOD
int parse(const char *s, int *out);  // returns 0 on success, errno-like on failure
if (parse(s, &v) != 0) { /* handle */ }

// BAD
int parse(const char *s);            // throws on failure
try { v = parse(s); } catch (...) { /* handle */ }
```

#### No function/operator overloading, no default arguments

```cpp
// GOOD
void draw_point(Point p);
void draw_line(Point a, Point b);
int  add_i(int a, int b);
float add_f(float a, float b);
int  scale(int x, int k);            // caller passes k explicitly

// BAD
void draw(Point);
void draw(Point, Point);             // function overload
int  operator+(MyT, MyT);            // operator overload
int  scale(int x, int k = 1);        // default argument
```

#### No user-defined conversions / literals

```cpp
// BAD
struct Money { operator double() const; };          // conversion overload
size_t operator""_kb(unsigned long long n);         // user-defined literal
```

#### Explicit `this->` and `Class::` qualifiers

```cpp
// GOOD (when methods are unavoidable)
int Widget::value() const { return this->x; }
int n = Widget::kCount;

// BAD
int Widget::value() const { return x; }              // implicit this
int n = kCount;                                      // implicit static qualifier
```

#### Plain `enum`, not `enum class`

```cpp
// GOOD
enum Color { COLOR_RED, COLOR_GREEN };

// BAD
enum class Color { Red, Green };
```

#### Avoid namespaces (keep shallow if used at all)

```cpp
// GOOD
int audio_buffer_size(void);

// BAD
namespace audio { namespace buffer { int size(); } }
```

#### No `mutable`

```cpp
// BAD
struct Cache { mutable int hits; };
```

### Pragmatism

When interfacing with an API that forces non-Orthodox features (e.g. Clang/LLVM, Qt), match the API's idioms locally; keep the violation as narrow as possible. Match surrounding style in any existing file.

### Suppression

In codebases enforced by an Orthodox C++ tool, suppress a single line with a trailing comment naming the rule, e.g.:

```cpp
static_cast<int>(x);  /* HERESY(static-cast) */
```

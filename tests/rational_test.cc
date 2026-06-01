/* rational_test.cc — exact-rational primitive (Stream B.0).
 *
 * Covers: normalization (gcd, positive denominator), the four ops as exact
 * identities, integer predicates, div-by-zero + invalid propagation, int64
 * overflow flagged (not silently wrapped), and a property check of add/sub/mul
 * against an independent __int128 reduced reference over random small operands.
 *
 * The __int128 reference oracle and the property test that uses it are compiled
 * only on GCC/Clang (where __SIZEOF_INT128__ is defined).  On MSVC the other
 * tests still run, exercising the portable emulation.
 */
#include "rational.h"

#include <gtest/gtest.h>
#include <stdint.h>

namespace {

#if defined(__SIZEOF_INT128__)
/* Independent reference: reduce num/den (given in __int128) to lowest terms,
 * positive denominator. Returns false if it does not fit int64 (caller then
 * expects the primitive to report invalid). */
bool ref_reduce(__int128 num, __int128 den, int64_t* on, int64_t* od) {
  if (den == 0) return false;
  if (den < 0) {
    num = -num;
    den = -den;
  }
  __int128 a = num < 0 ? -num : num;
  __int128 b = den;
  while (b != 0) {
    __int128 t = a % b;
    a = b;
    b = t;
  }
  if (a > 1) {
    num /= a;
    den /= a;
  }
  if (num < (__int128)INT64_MIN || num > (__int128)INT64_MAX) return false;
  if (den < (__int128)INT64_MIN || den > (__int128)INT64_MAX) return false;
  *on = (int64_t)num;
  *od = (int64_t)den;
  return true;
}
#endif /* defined(__SIZEOF_INT128__) */

struct Rat make(int64_t n, int64_t d) {
  return rat_div(rat_from_i64(n), rat_from_i64(d));
}

}  // namespace

TEST(Rational, NormalizationReducesAndSignsDenominator) {
  struct Rat r = make(6, 8);
  EXPECT_TRUE(rat_ok(r));
  EXPECT_EQ(r.num, 3);
  EXPECT_EQ(r.den, 4);

  struct Rat neg = make(1, -2);
  EXPECT_EQ(neg.num, -1);
  EXPECT_EQ(neg.den, 2); /* denominator always positive */

  struct Rat z = make(0, 5);
  EXPECT_EQ(z.num, 0);
  EXPECT_EQ(z.den, 1); /* zero canonicalizes to 0/1 */
  EXPECT_TRUE(rat_is_zero(z));
}

TEST(Rational, ExactArithmeticIdentities) {
  struct Rat s = rat_add(make(1, 2), make(1, 3)); /* 5/6 */
  EXPECT_EQ(s.num, 5);
  EXPECT_EQ(s.den, 6);

  struct Rat d = rat_sub(make(1, 2), make(1, 3)); /* 1/6 */
  EXPECT_EQ(d.num, 1);
  EXPECT_EQ(d.den, 6);

  struct Rat m = rat_mul(make(2, 3), make(3, 4)); /* 1/2 */
  EXPECT_EQ(m.num, 1);
  EXPECT_EQ(m.den, 2);

  struct Rat q = rat_div(make(1, 2), make(3, 4)); /* 2/3 */
  EXPECT_EQ(q.num, 2);
  EXPECT_EQ(q.den, 3);

  struct Rat whole = rat_add(make(1, 2), make(1, 2)); /* 1 */
  EXPECT_TRUE(rat_eq_i64(whole, 1));
}

TEST(Rational, IntegerPredicates) {
  EXPECT_TRUE(rat_eq_i64(rat_from_i64(7), 7));
  EXPECT_FALSE(rat_eq_i64(make(3, 2), 1));
  EXPECT_FALSE(rat_eq_i64(make(3, 2), 2));
}

TEST(Rational, DivByZeroAndInvalidPropagation) {
  struct Rat bad = rat_div(rat_from_i64(1), rat_from_i64(0));
  EXPECT_FALSE(rat_ok(bad));

  /* invalid propagates through every op */
  EXPECT_FALSE(rat_ok(rat_add(bad, make(1, 2))));
  EXPECT_FALSE(rat_ok(rat_sub(make(1, 2), bad)));
  EXPECT_FALSE(rat_ok(rat_mul(bad, make(1, 2))));
  EXPECT_FALSE(rat_ok(rat_div(make(1, 2), bad)));
}

TEST(Rational, OverflowFlaggedNotWrapped) {
  /* (INT64_MAX)/1 + 1/2 = (2*MAX+1)/2 — numerator cannot fit int64 → invalid */
  struct Rat big = rat_add(rat_from_i64(INT64_MAX), make(1, 2));
  EXPECT_FALSE(rat_ok(big));

  /* product of two large coprime values overflows after reduction → invalid */
  struct Rat p =
      rat_mul(rat_from_i64(3037000500LL), rat_from_i64(3037000500LL));
  EXPECT_FALSE(rat_ok(p)); /* ~9.22e18 > INT64_MAX */

  /* but a large intermediate that REDUCES to a small value is kept */
  struct Rat keep = rat_div(rat_from_i64(1000000000000LL),
                            rat_from_i64(2000000000000LL)); /* 1/2 */
  EXPECT_TRUE(rat_ok(keep));
  EXPECT_EQ(keep.num, 1);
  EXPECT_EQ(keep.den, 2);
}

#if defined(__SIZEOF_INT128__)
TEST(Rational, PropertyVsInt128Reference) {
  uint64_t seed = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 20000; ++i) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    int64_t n1 = (int64_t)((seed >> 8) % 101) - 50;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    int64_t d1 = (int64_t)((seed >> 8) % 50) + 1;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    int64_t n2 = (int64_t)((seed >> 8) % 101) - 50;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    int64_t d2 = (int64_t)((seed >> 8) % 50) + 1;

    struct Rat a = make(n1, d1);
    struct Rat b = make(n2, d2);
    ASSERT_TRUE(rat_ok(a));
    ASSERT_TRUE(rat_ok(b));

    /* add */
    int64_t rn = 0;
    int64_t rd = 0;
    ASSERT_TRUE(ref_reduce((__int128)a.num * b.den + (__int128)b.num * a.den,
                           (__int128)a.den * b.den, &rn, &rd));
    struct Rat add = rat_add(a, b);
    ASSERT_TRUE(rat_ok(add));
    EXPECT_EQ(add.num, rn);
    EXPECT_EQ(add.den, rd);

    /* sub */
    ASSERT_TRUE(ref_reduce((__int128)a.num * b.den - (__int128)b.num * a.den,
                           (__int128)a.den * b.den, &rn, &rd));
    struct Rat sub = rat_sub(a, b);
    ASSERT_TRUE(rat_ok(sub));
    EXPECT_EQ(sub.num, rn);
    EXPECT_EQ(sub.den, rd);

    /* mul */
    ASSERT_TRUE(
        ref_reduce((__int128)a.num * b.num, (__int128)a.den * b.den, &rn, &rd));
    struct Rat mul = rat_mul(a, b);
    ASSERT_TRUE(rat_ok(mul));
    EXPECT_EQ(mul.num, rn);
    EXPECT_EQ(mul.den, rd);
  }
}
#endif /* defined(__SIZEOF_INT128__) */

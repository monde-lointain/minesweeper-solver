/* rational.h — exact rational over int64 for the Gaussian-reduction path.
 *
 * Normalized invariant: den > 0 and gcd(|num|, den) == 1. den == 0 is the
 * INVALID / overflow sentinel; once a value goes invalid it propagates through
 * every op, so the reduction driver can detect it and bail to the naive
 * fallback (we never emit a wrong marginal from overflow).
 *
 * All arithmetic is done in __int128 then range-checked back into int64 AFTER
 * gcd reduction — so a result that reduces to a small fraction is kept even if
 * the unreduced intermediate would not fit. For normalized int64 operands the
 * __int128 intermediates cannot themselves overflow (INT64_MAX^2 * 2 <
 * INT128_MAX), so __int128 is a sufficient working type.
 *
 * Orthodox C++: POD struct, plain functions, C headers. __int128 is a narrowly
 * scoped compiler builtin (like the threading carve-out), used only here.
 */
#ifndef SOLVER_ENGINE_RATIONAL_H
#define SOLVER_ENGINE_RATIONAL_H

#include <stdint.h>

struct Rat {
  int64_t num;
  int64_t den;
};

static inline struct Rat rat_invalid(void) {
  struct Rat r;
  r.num = 0;
  r.den = 0;
  return r;
}

static inline bool rat_ok(struct Rat a) { return a.den != 0; }

static inline struct Rat rat_from_i64(int64_t v) {
  struct Rat r;
  r.num = v;
  r.den = 1;
  return r;
}

/* Normalize a num/den pair given in __int128 into a canonical Rat, or invalid
 * on a zero denominator or an int64 range overflow after reduction. */
static inline struct Rat rat_norm(__int128 num, __int128 den) {
  if (den == 0) {
    return rat_invalid();
  }
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
  const __int128 lo = (__int128)INT64_MIN;
  const __int128 hi = (__int128)INT64_MAX;
  if (num < lo || num > hi || den < lo || den > hi) {
    return rat_invalid();
  }
  struct Rat r;
  r.num = (int64_t)num;
  r.den = (int64_t)den;
  return r;
}

static inline struct Rat rat_add(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  __int128 num = (__int128)a.num * b.den + (__int128)b.num * a.den;
  __int128 den = (__int128)a.den * b.den;
  return rat_norm(num, den);
}

static inline struct Rat rat_sub(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  __int128 num = (__int128)a.num * b.den - (__int128)b.num * a.den;
  __int128 den = (__int128)a.den * b.den;
  return rat_norm(num, den);
}

static inline struct Rat rat_mul(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  return rat_norm((__int128)a.num * b.num, (__int128)a.den * b.den);
}

static inline struct Rat rat_div(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b) || b.num == 0) {
    return rat_invalid();
  }
  return rat_norm((__int128)a.num * b.den, (__int128)a.den * b.num);
}

static inline bool rat_is_zero(struct Rat a) { return rat_ok(a) && a.num == 0; }

static inline bool rat_is_int(struct Rat a) { return rat_ok(a) && a.den == 1; }

/* True iff a is exactly the integer v. */
static inline bool rat_eq_i64(struct Rat a, int64_t v) {
  return rat_ok(a) && a.den == 1 && a.num == v;
}

#endif /* SOLVER_ENGINE_RATIONAL_H */

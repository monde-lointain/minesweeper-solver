/* rational.h — exact rational over int64 for the Gaussian-reduction path.
 *
 * Normalized invariant: den > 0 and gcd(|num|, den) == 1. den == 0 is the
 * INVALID / overflow sentinel; once a value goes invalid it propagates through
 * every op, so the reduction driver can detect it and bail to the naive
 * fallback (we never emit a wrong marginal from overflow).
 *
 * All arithmetic is done in a 128-bit working type then range-checked back
 * into int64 AFTER gcd reduction — so a result that reduces to a small
 * fraction is kept even if the unreduced intermediate would not fit. For
 * normalized int64 operands the 128-bit intermediates cannot themselves
 * overflow (INT64_MAX^2 * 2 < INT128_MAX), so 128 bits is a sufficient
 * working type.
 *
 * Orthodox C++: POD struct, plain functions, C headers.
 *
 * Portability: on GCC/Clang (where __SIZEOF_INT128__ is defined and
 * RAT_FORCE_EMULATED_I128 is not set) we use the native __int128 builtin
 * directly.  On MSVC (or when RAT_FORCE_EMULATED_I128 is defined for testing)
 * we use a portable software emulation in two uint64_t limbs.
 */
#ifndef SOLVER_ENGINE_RATIONAL_H
#define SOLVER_ENGINE_RATIONAL_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * 128-bit working type: native or emulated
 * ---------------------------------------------------------------------- */

#if defined(__SIZEOF_INT128__) && !defined(RAT_FORCE_EMULATED_I128)

/* Native path: GCC / Clang on 64-bit. */

/* __extension__ silences -Wpedantic (ISO C++ has no __int128). */
__extension__ typedef __int128 RatI128;

__extension__ static inline RatI128 i128_from_i64(int64_t v) {
  return (RatI128)v;
}

/* Multiply two int64 values into a 128-bit result. */
__extension__ static inline RatI128 i128_mul_i64(int64_t a, int64_t b) {
  return (RatI128)a * (RatI128)b;
}

__extension__ static inline RatI128 i128_add(RatI128 a, RatI128 b) {
  return a + b;
}

__extension__ static inline RatI128 i128_sub(RatI128 a, RatI128 b) {
  return a - b;
}

__extension__ static inline RatI128 i128_neg(RatI128 a) { return -a; }

__extension__ static inline RatI128 i128_div(RatI128 a, RatI128 b) {
  return a / b;
}

__extension__ static inline RatI128 i128_mod(RatI128 a, RatI128 b) {
  return a % b;
}

__extension__ static inline int i128_lt_zero(RatI128 a) { return a < 0; }

__extension__ static inline int i128_eq_zero(RatI128 a) { return a == 0; }

/* Returns -1/0/1 for a < b / a == b / a > b. */
__extension__ static inline int i128_cmp(RatI128 a, RatI128 b) {
  return (a > b) - (a < b);
}

/* True iff a > 1 (used in gcd branch). */
__extension__ static inline int i128_gt_one(RatI128 a) { return a > 1; }

/* True iff a is in [INT64_MIN, INT64_MAX]. */
__extension__ static inline int i128_fits_i64(RatI128 a) {
  return a >= (RatI128)INT64_MIN && a <= (RatI128)INT64_MAX;
}

__extension__ static inline int64_t i128_to_i64(RatI128 a) {
  return (int64_t)a;
}

/* Absolute value as RatI128. */
__extension__ static inline RatI128 i128_abs(RatI128 a) {
  return a < 0 ? -a : a;
}

#else /* ------------------------------------------------------------------- */
/* Emulated path: MSVC or RAT_FORCE_EMULATED_I128.
 *
 * Representation: signed 128-bit two's complement stored as two uint64_t
 * limbs: lo = bits[63:0], hi = bits[127:64] (sign-extended in hi).
 * The value is:  (int64_t)hi * 2^64  +  (uint64_t)lo
 */

struct RatI128 {
  uint64_t lo; /* bits [63:0]  */
  uint64_t hi; /* bits [127:64] — sign bit in bit 63 of hi */
};

/* Sign bit. */
static inline int i128_negative(struct RatI128 a) {
  return (int64_t)a.hi < 0;
}

static inline struct RatI128 i128_from_i64(int64_t v) {
  struct RatI128 r;
  r.lo = (uint64_t)v;
  r.hi = (uint64_t)((int64_t)v >> 63); /* sign-extend */
  return r;
}

/* Negate in two's complement. */
static inline struct RatI128 i128_neg(struct RatI128 a) {
  struct RatI128 r;
  r.lo = ~a.lo + 1u;
  r.hi = ~a.hi + (r.lo == 0u ? 1u : 0u);
  return r;
}

/* Absolute value as an unsigned 128-bit pair {ulo, uhi}.
 * Returns 1 if the original was negative (so caller can correct sign). */
static inline int i128_to_umag(struct RatI128 a, uint64_t* ulo,
                                uint64_t* uhi) {
  if (i128_negative(a)) {
    struct RatI128 pos = i128_neg(a);
    *ulo = pos.lo;
    *uhi = pos.hi;
    return 1;
  }
  *ulo = a.lo;
  *uhi = a.hi;
  return 0;
}

static inline struct RatI128 i128_add(struct RatI128 a, struct RatI128 b) {
  struct RatI128 r;
  r.lo = a.lo + b.lo;
  uint64_t carry = (r.lo < a.lo) ? 1u : 0u;
  r.hi = a.hi + b.hi + carry;
  return r;
}

static inline struct RatI128 i128_sub(struct RatI128 a, struct RatI128 b) {
  return i128_add(a, i128_neg(b));
}

/* Multiply two uint64 → 128-bit unsigned result, via four 32×32 products. */
static inline void u128_mul_u64(uint64_t a, uint64_t b, uint64_t* lo,
                                 uint64_t* hi) {
  uint64_t a_lo = a & 0xFFFFFFFFu;
  uint64_t a_hi = a >> 32;
  uint64_t b_lo = b & 0xFFFFFFFFu;
  uint64_t b_hi = b >> 32;

  uint64_t p0 = a_lo * b_lo;
  uint64_t p1 = a_lo * b_hi;
  uint64_t p2 = a_hi * b_lo;
  uint64_t p3 = a_hi * b_hi;

  uint64_t mid = p1 + p2; /* may carry */
  uint64_t mid_carry = (mid < p1) ? 1u : 0u;

  *lo = p0 + (mid << 32);
  uint64_t lo_carry = (*lo < p0) ? 1u : 0u;
  *hi = p3 + (mid >> 32) + (mid_carry << 32) + lo_carry;
}

/* Multiply two int64 → signed 128-bit result. */
static inline struct RatI128 i128_mul_i64(int64_t sa, int64_t sb) {
  /* compute magnitude, track sign */
  uint64_t ua = (uint64_t)(sa < 0 ? -sa : sa); /* HERESY(C-cast-neg): safe */
  uint64_t ub = (uint64_t)(sb < 0 ? -sb : sb);
  int neg = (sa < 0) ^ (sb < 0);

  uint64_t lo, hi;
  u128_mul_u64(ua, ub, &lo, &hi);

  struct RatI128 r;
  r.lo = lo;
  r.hi = hi;
  if (neg) {
    r = i128_neg(r);
  }
  return r;
}

/* ---- Unsigned 128-bit divide/modulo (helper) ----------------------------
 * Binary long-division: quotient and remainder.
 * Divisor must be non-zero. */
static inline void u128_divmod(uint64_t nlo, uint64_t nhi, uint64_t dlo,
                                uint64_t dhi, uint64_t* qlo, uint64_t* qhi,
                                uint64_t* rlo, uint64_t* rhi) {
  /* Handle the common case: divisor fits in 64 bits and dividend < 2^128. */
  *qlo = 0u;
  *qhi = 0u;
  *rlo = 0u;
  *rhi = 0u;

  /* Shift-and-subtract binary long division on 128-bit operands. */
  uint64_t nlo_cur = nlo;
  uint64_t nhi_cur = nhi;
  /* We accumulate quotient bits from MSB down. */
  /* Iterate over all 128 bit positions. */
  int bit;
  for (bit = 127; bit >= 0; --bit) {
    /* remainder <<= 1 */
    uint64_t new_rhi = (*rhi << 1) | (*rlo >> 63);
    uint64_t new_rlo = *rlo << 1;

    /* bring down one bit from numerator (bit 'bit' of n) */
    uint64_t nbit;
    if (bit >= 64) {
      nbit = (nhi_cur >> (bit - 64)) & 1u;
    } else {
      nbit = (nlo_cur >> bit) & 1u;
    }
    new_rlo |= nbit;

    /* if remainder >= divisor, subtract and set quotient bit */
    if (new_rhi > dhi || (new_rhi == dhi && new_rlo >= dlo)) {
      /* remainder -= divisor */
      uint64_t borrow = (new_rlo < dlo) ? 1u : 0u;
      new_rlo -= dlo;
      new_rhi -= dhi + borrow;

      /* set quotient bit */
      if (bit >= 64) {
        *qhi |= (uint64_t)1u << (bit - 64);
      } else {
        *qlo |= (uint64_t)1u << bit;
      }
    }
    *rlo = new_rlo;
    *rhi = new_rhi;
  }
  /* suppress unused warning: nhi_cur, nlo_cur were read in loop */
  (void)nhi_cur;
  (void)nlo_cur;
}

/* Signed 128 / signed 128, truncation toward zero (C semantics). */
static inline struct RatI128 i128_div(struct RatI128 a, struct RatI128 b) {
  uint64_t alo, ahi, blo, bhi;
  int aneg = i128_to_umag(a, &alo, &ahi);
  int bneg = i128_to_umag(b, &blo, &bhi);

  uint64_t qlo, qhi, rlo, rhi;
  u128_divmod(alo, ahi, blo, bhi, &qlo, &qhi, &rlo, &rhi);

  struct RatI128 q;
  q.lo = qlo;
  q.hi = qhi;
  if (aneg ^ bneg) {
    q = i128_neg(q);
  }
  return q;
}

/* Signed 128 % signed 128, sign matches dividend (C semantics). */
static inline struct RatI128 i128_mod(struct RatI128 a, struct RatI128 b) {
  uint64_t alo, ahi, blo, bhi;
  int aneg = i128_to_umag(a, &alo, &ahi);
  int bneg = i128_to_umag(b, &blo, &bhi);
  (void)bneg;

  uint64_t qlo, qhi, rlo, rhi;
  u128_divmod(alo, ahi, blo, bhi, &qlo, &qhi, &rlo, &rhi);
  (void)qlo;
  (void)qhi;

  struct RatI128 r;
  r.lo = rlo;
  r.hi = rhi;
  if (aneg && (rlo != 0u || rhi != 0u)) {
    r = i128_neg(r);
  }
  return r;
}

static inline int i128_lt_zero(struct RatI128 a) { return i128_negative(a); }

static inline int i128_eq_zero(struct RatI128 a) {
  return a.lo == 0u && a.hi == 0u;
}

/* Returns -1/0/+1 for a < b / == / >. */
static inline int i128_cmp(struct RatI128 a, struct RatI128 b) {
  struct RatI128 diff = i128_sub(a, b);
  if (i128_eq_zero(diff)) return 0;
  return i128_negative(diff) ? -1 : 1;
}

/* True iff a > 1. */
static inline int i128_gt_one(struct RatI128 a) {
  struct RatI128 one = i128_from_i64(1);
  return i128_cmp(a, one) > 0;
}

/* True iff a in [INT64_MIN, INT64_MAX]. */
static inline int i128_fits_i64(struct RatI128 a) {
  struct RatI128 lo = i128_from_i64(INT64_MIN);
  struct RatI128 hi = i128_from_i64(INT64_MAX);
  return i128_cmp(a, lo) >= 0 && i128_cmp(a, hi) <= 0;
}

static inline int64_t i128_to_i64(struct RatI128 a) {
  return (int64_t)a.lo; /* only valid after i128_fits_i64 check */
}

static inline struct RatI128 i128_abs(struct RatI128 a) {
  return i128_negative(a) ? i128_neg(a) : a;
}

#endif /* __SIZEOF_INT128__ && !RAT_FORCE_EMULATED_I128 */

/* -------------------------------------------------------------------------
 * Rational arithmetic
 * ---------------------------------------------------------------------- */

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

static inline int rat_ok(struct Rat a) { return a.den != 0; }

static inline struct Rat rat_from_i64(int64_t v) {
  struct Rat r;
  r.num = v;
  r.den = 1;
  return r;
}

/* Normalize a num/den pair (given as RatI128) into a canonical Rat, or
 * invalid on a zero denominator or int64 range overflow after reduction. */
static inline struct Rat rat_norm(RatI128 num, RatI128 den) {
  if (i128_eq_zero(den)) {
    return rat_invalid();
  }
  if (i128_lt_zero(den)) {
    num = i128_neg(num);
    den = i128_neg(den);
  }
  /* Fast path: both operands already fit int64 (the common case -- RREF
   * residuals of small {0,1} systems). Reduce via a native 64-bit Euclidean
   * gcd: a single hardware div per step on x86-64 (clang and MSVC), so no
   * __modti3 and no intrinsics. The 128-bit gcd below handles overflow
   * operands. Byte-identical to the slow path (gcd unique, /1 a no-op). */
  if (i128_fits_i64(num) && i128_fits_i64(den)) {
    int64_t n = i128_to_i64(num);
    int64_t d = i128_to_i64(den); /* d > 0 (sign moved to num above) */
    /* |n| as unsigned; 0u - (uint64_t)n is correct even for INT64_MIN */
    uint64_t ga = n < 0 ? 0u - (uint64_t)n : (uint64_t)n;
    uint64_t gb = (uint64_t)d;
    while (ga != 0u) {
      uint64_t t = gb % ga;
      gb = ga;
      ga = t;
    }
    /* gb = gcd(|n|, d) >= 1; both divisions are exact and in range */
    struct Rat r;
    r.num = n / (int64_t)gb;
    r.den = d / (int64_t)gb;
    return r;
  }
  /* gcd via Euclidean algorithm */
  RatI128 a = i128_abs(num);
  RatI128 b = den;
  while (!i128_eq_zero(b)) {
    RatI128 t = i128_mod(a, b);
    a = b;
    b = t;
  }
  if (i128_gt_one(a)) {
    num = i128_div(num, a);
    den = i128_div(den, a);
  }
  if (!i128_fits_i64(num) || !i128_fits_i64(den)) {
    return rat_invalid();
  }
  struct Rat r;
  r.num = i128_to_i64(num);
  r.den = i128_to_i64(den);
  return r;
}

static inline struct Rat rat_add(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  RatI128 num =
      i128_add(i128_mul_i64(a.num, b.den), i128_mul_i64(b.num, a.den));
  RatI128 den = i128_mul_i64(a.den, b.den);
  return rat_norm(num, den);
}

static inline struct Rat rat_sub(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  RatI128 num =
      i128_sub(i128_mul_i64(a.num, b.den), i128_mul_i64(b.num, a.den));
  RatI128 den = i128_mul_i64(a.den, b.den);
  return rat_norm(num, den);
}

static inline struct Rat rat_mul(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b)) {
    return rat_invalid();
  }
  return rat_norm(i128_mul_i64(a.num, b.num), i128_mul_i64(a.den, b.den));
}

static inline struct Rat rat_div(struct Rat a, struct Rat b) {
  if (!rat_ok(a) || !rat_ok(b) || b.num == 0) {
    return rat_invalid();
  }
  return rat_norm(i128_mul_i64(a.num, b.den), i128_mul_i64(a.den, b.num));
}

static inline int rat_is_zero(struct Rat a) {
  return rat_ok(a) && a.num == 0;
}

/* True iff a is exactly the integer v. */
static inline int rat_eq_i64(struct Rat a, int64_t v) {
  return rat_ok(a) && a.den == 1 && a.num == v;
}

/* sign(a - v): -1, 0, or +1. Requires rat_ok(a). Computed in 128-bit so it
 * cannot overflow for any normalized Rat and small v. */
static inline int rat_cmp_i64(struct Rat a, int64_t v) {
  RatI128 t = i128_sub(i128_from_i64(a.num),
                       i128_mul_i64(v, a.den));
  if (i128_lt_zero(t)) return -1;
  if (i128_eq_zero(t)) return 0;
  return 1;
}

#endif /* SOLVER_ENGINE_RATIONAL_H */

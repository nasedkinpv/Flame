// Offline differential test for src/dk2/Mat3x3f.cpp (existing SSE2 rewrites of
// DKII.EXE 00594CB0 sub_594CB0 (matmul), 00594DB0 multiplyVec, 00594E10,
// 00594E70 (mat-vec rows vs columns), 00594ED0 multiply (scalar),
// 00594F30 sub_594F30 (transpose)).
//
// The impls use explicit _mm_mul_ps/_mm_add_ps (no FMA), one single-precision
// rounding per op. The references mirror that exact evaluation order with
// explicit float intermediates; build with -ffp-contract=off so the reference
// does not fuse mul+add into FMA. Under those terms bit-exact equality holds.
//
// LCG-driven value pool includes 0, -0, 1, -1, small, large, denormals, inf,
// nan. Aliasing variants: output distinct, output==this, output==right (matmul).
//
// Build & run (Apple Silicon via Rosetta):
//   clang++ -arch x86_64 -O2 -std=c++17 -ffp-contract=off -I tests/mat3x3f_difftest \
//       -o /tmp/mat3x3f_difftest tests/mat3x3f_difftest/mat3x3f_difftest.cpp
//   /tmp/mat3x3f_difftest
#include "../../src/dk2/Mat3x3f.cpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using dk2::Mat3x3f;
using dk2::Vec3f;

static bool bitEq(float a, float b) {
    uint32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    return ia == ib || (std::isnan(a) && std::isnan(b));
}
static bool eqM(const Mat3x3f &a, const Mat3x3f &b) {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        if (!bitEq(a.m[i][j], b.m[i][j])) return false;
    return true;
}
static bool eqV(const Vec3f &a, const Vec3f &b) {
    return bitEq(a.x, b.x) && bitEq(a.y, b.y) && bitEq(a.z, b.z);
}

// combineRows per component: ((m[0][c]*x + m[1][c]*y) + m[2][c]*z)
static float rowDot(const Mat3x3f &t, int c, float x, float y, float z) {
    float t1 = t.m[0][c] * x;
    float t2 = t.m[1][c] * y;
    float s = t1 + t2;
    float t3 = t.m[2][c] * z;
    return s + t3;
}
static Vec3f refMulVec(const Mat3x3f &t, const Vec3f &v) {
    return {rowDot(t, 0, v.x, v.y, v.z), rowDot(t, 1, v.x, v.y, v.z), rowDot(t, 2, v.x, v.y, v.z)};
}
// combineColumns per component: ((m[c][0]*x + m[c][1]*y) + m[c][2]*z)
static float colDot(const Mat3x3f &t, int c, float x, float y, float z) {
    float t1 = t.m[c][0] * x;
    float t2 = t.m[c][1] * y;
    float s = t1 + t2;
    float t3 = t.m[c][2] * z;
    return s + t3;
}
static Vec3f refFunE70(const Mat3x3f &t, const Vec3f &v) {
    return {colDot(t, 0, v.x, v.y, v.z), colDot(t, 1, v.x, v.y, v.z), colDot(t, 2, v.x, v.y, v.z)};
}
static Mat3x3f refMatmul(const Mat3x3f &t, const Mat3x3f &right) {
    Mat3x3f o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o.m[r][c] = rowDot(t, c, right.m[r][0], right.m[r][1], right.m[r][2]);
    return o;
}
static Mat3x3f refMulScalar(const Mat3x3f &t, float s) {
    Mat3x3f o;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o.m[i][j] = t.m[i][j] * s;
    return o;
}
static Mat3x3f refTranspose(const Mat3x3f &t) {
    Mat3x3f o;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o.m[i][j] = t.m[j][i];
    return o;
}

int main() {
    const float pool[] = {
        0.f, -0.f, 1.f, -1.f, 0.5f, -0.5f, 2.f, 3.14159265f,
        -123.456f, 1e-20f, -1e-20f, 1e-38f /*denormal*/, -1e-38f,
        1e20f, -1e20f, 65535.f, INFINITY, -INFINITY, NAN};
    const int NPOOL = sizeof(pool) / sizeof(pool[0]);
    uint32_t state = 0x12345678u;
    auto next = [&]() -> float {  // LCG over the pool
        state = state * 1103515245u + 12345u;
        return pool[state % NPOOL];
    };

    long n = 0;
    const long ITERS = 200000;
    for (long it = 0; it < ITERS; ++it) {
        Mat3x3f A, B;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { A.m[i][j] = next(); B.m[i][j] = next(); }
        Vec3f v{next(), next(), next()};

        // multiplyVec / sub_594E10: out = A*v (rows)
        { Vec3f vorig = v; Vec3f e = refMulVec(A, v);
          Vec3f out{9,9,9}; Mat3x3f t = A; Vec3f *r = t.multiplyVec(&out, &v);
          assert(r == &out && eqV(out, e) && eqV(v, vorig)); }
        { Mat3x3f t = A; Vec3f vv = v; t.sub_594E10(&vv, &vv); assert(eqV(vv, refMulVec(A, v))); }  // out==input
        // fun_594E70: columns
        { Vec3f e = refFunE70(A, v);
          Vec3f out{9,9,9}; Mat3x3f t = A; Vec3f *r = t.fun_594E70(&v, &out);
          assert(r == &out && eqV(out, e)); }
        // multiply scalar
        { float s = next(); Mat3x3f e = refMulScalar(A, s);
          Mat3x3f out, t = A; Mat3x3f *r = t.multiply(&out, s);
          assert(r == &out && eqM(out, e)); }
        { float s = next(); Mat3x3f t = A; t.multiply(&t, s); assert(eqM(t, refMulScalar(A, s))); }  // out==this
        // transpose
        { Mat3x3f e = refTranspose(A);
          Mat3x3f out, t = A; Mat3x3f *r = t.sub_594F30(&out);
          assert(r == &out && eqM(out, e)); }
        { Mat3x3f t = A; t.sub_594F30(&t); assert(eqM(t, refTranspose(A))); }  // out==this
        // matmul A *right
        { Mat3x3f e = refMatmul(A, B);
          Mat3x3f out, t = A, r = B; Mat3x3f *res = t.sub_594CB0(&out, &r);
          assert(res == &out && eqM(out, e) && eqM(r, B)); }
        { Mat3x3f t = A, r = B; t.sub_594CB0(&t, &r); assert(eqM(t, refMatmul(A, B))); }      // out==this
        { Mat3x3f t = A, r = B; t.sub_594CB0(&r, &r); assert(eqM(r, refMatmul(A, B))); }        // out==right
        ++n;
    }
    printf("OK: %ld iterations x 9 cases, 6 Mat3x3f methods bit-exact (incl. nan/inf/denormal + aliasing)\n", n);
    return 0;
}

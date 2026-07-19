#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"

#include <cmath>
#include <emmintrin.h>


namespace {

__m128 combineRows(const float (&m)[3][3], float x, float y, float z) {
    const __m128 xyzMask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    const __m128 row0 = _mm_and_ps(_mm_loadu_ps(&m[0][0]), xyzMask);
    const __m128 row1 = _mm_and_ps(_mm_loadu_ps(&m[1][0]), xyzMask);
    const __m128 row2 = _mm_set_ps(0.0f, m[2][2], m[2][1], m[2][0]);
    return _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(row0, _mm_set1_ps(x)),
                       _mm_mul_ps(row1, _mm_set1_ps(y))),
            _mm_mul_ps(row2, _mm_set1_ps(z)));
}

__m128 combineColumns(const float (&m)[3][3], float x, float y, float z) {
    const __m128 column0 = _mm_set_ps(0.0f, m[2][0], m[1][0], m[0][0]);
    const __m128 column1 = _mm_set_ps(0.0f, m[2][1], m[1][1], m[0][1]);
    const __m128 column2 = _mm_set_ps(0.0f, m[2][2], m[1][2], m[0][2]);
    return _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(column0, _mm_set1_ps(x)),
                       _mm_mul_ps(column1, _mm_set1_ps(y))),
            _mm_mul_ps(column2, _mm_set1_ps(z)));
}

void storeVec3(dk2::Vec3f *output, __m128 value) {
    _mm_storel_pi(reinterpret_cast<__m64 *>(output), value);
    _mm_store_ss(&output->z,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

void storeRow(float *output, __m128 value) {
    _mm_storel_pi(reinterpret_cast<__m64 *>(output), value);
    _mm_store_ss(output + 2,
                 _mm_shuffle_ps(value, value, _MM_SHUFFLE(2, 2, 2, 2)));
}

__m128 loadRow(const float *input) {
    return _mm_set_ps(0.0f, input[2], input[1], input[0]);
}

}


dk2::Mat3x3f *dk2::Mat3x3f::sub_594CB0(
        Mat3x3f *output, Mat3x3f *right) {
    Mat3x3f result;
    for (int row = 0; row < 3; ++row) {
        storeRow(result.m[row],
                 combineRows(m,
                             right->m[row][0],
                             right->m[row][1],
                             right->m[row][2]));
    }
    *output = result;
    return output;
}


dk2::Vec3f *dk2::Mat3x3f::multiplyVec(Vec3f *output, Vec3f *input) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineRows(m, x, y, z));
    return output;
}


dk2::Vec3f *dk2::Mat3x3f::sub_594E10(Vec3f *input, Vec3f *output) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineRows(m, x, y, z));
    return output;
}


dk2::Vec3f *dk2::Mat3x3f::fun_594E70(Vec3f *input, Vec3f *output) {
    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    storeVec3(output, combineColumns(m, x, y, z));
    return output;
}


dk2::Mat3x3f *dk2::Mat3x3f::multiply(Mat3x3f *output, float scalar) {
    const float *source = &m[0][0];
    float *destination = &output->m[0][0];
    const __m128 scale = _mm_set1_ps(scalar);
    _mm_storeu_ps(destination, _mm_mul_ps(_mm_loadu_ps(source), scale));
    _mm_storeu_ps(destination + 4,
                  _mm_mul_ps(_mm_loadu_ps(source + 4), scale));
    _mm_store_ss(destination + 8,
                 _mm_mul_ss(_mm_load_ss(source + 8), scale));
    return output;
}


dk2::Mat3x3f *dk2::Mat3x3f::sub_594F30(Mat3x3f *output) {
    __m128 row0 = loadRow(m[0]);
    __m128 row1 = loadRow(m[1]);
    __m128 row2 = loadRow(m[2]);
    __m128 row3 = _mm_setzero_ps();
    _MM_TRANSPOSE4_PS(row0, row1, row2, row3);

    Mat3x3f result;
    storeRow(result.m[0], row0);
    storeRow(result.m[1], row1);
    storeRow(result.m[2], row2);
    *output = result;
    return output;
}


// 0041C580: build an axis rotation matrix. The original uses x87 fsin/fcos,
// which Rosetta emulates via slow helpers anyway; sinf/cosf may differ in the
// last ulp from fsin/fcos, which only feeds rendering transforms here.
int dk2::Mat3x3f::init_rotationMat(int axis, float angle) {
    const float c = cosf(angle);
    const float s = sinf(angle);
    switch (axis) {
    case 0:
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = c;    m[1][2] = -s;
        m[2][0] = 0.0f; m[2][1] = s;    m[2][2] = c;
        break;
    case 1:
        m[0][0] = c;    m[0][1] = 0.0f; m[0][2] = s;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
        m[2][0] = -s;   m[2][1] = 0.0f; m[2][2] = c;
        break;
    case 2:
        m[0][0] = c;    m[0][1] = -s;   m[0][2] = 0.0f;
        m[1][0] = s;    m[1][1] = c;    m[1][2] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;
        break;
    default:
        break;  // the original leaves the matrix untouched
    }
    return 0;
}

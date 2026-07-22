// Offline stub of the genapi-generated dk2/utils/Mat3x3f.h.
#pragma once
#include <cstdint>
namespace dk2 {
struct Vec3f;
#pragma pack(push, 1)
struct Mat3x3f {
    float m[3][3];
    int init_rotationMat(int axis, float angle);
    Mat3x3f *sub_594CB0(Mat3x3f *output, Mat3x3f *right);
    Vec3f *multiplyVec(Vec3f *output, Vec3f *input);
    Vec3f *sub_594E10(Vec3f *input, Vec3f *output);
    Vec3f *fun_594E70(Vec3f *input, Vec3f *output);
    Mat3x3f *multiply(Mat3x3f *output, float scalar);
    Mat3x3f *sub_594F30(Mat3x3f *output);
};
#pragma pack(pop)
static_assert(sizeof(Mat3x3f) == 0x24);
}

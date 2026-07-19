// Differential oracle for DKII.EXE 0058F030 (Obj58EF60::sub_58F030).
//
// Build this as a 32-bit .NET Framework executable on Windows and pass the
// path to an original Dungeon Keeper 2 v1.7 DKII-DX.exe.  The harness copies
// the leaf function into executable memory, recreates its lookup table, and
// compares it with the readable trilinear formulation below.  No game code or
// data is distributed with the test.
//
//   C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo \
//       /platform:x86 /optimize+ field_sampler_difftest.cs
//   field_sampler_difftest.exe C:\path\to\DKII-DX.exe

using System;
using System.IO;
using System.Runtime.InteropServices;

internal static class FieldSamplerDiffTest {
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint PageExecuteReadWrite = 0x40;
    private const uint PageReadWrite = 0x04;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAlloc(
        IntPtr address, UIntPtr size, uint allocationType, uint protect);

    [UnmanagedFunctionPointer(CallingConvention.ThisCall)]
    private delegate IntPtr OriginalSampler(
        IntPtr self, float x, float y, float z, IntPtr output);

    [StructLayout(LayoutKind.Explicit)]
    private struct FloatBits {
        [FieldOffset(0)] public float Value;
        [FieldOffset(0)] public int Bits;
    }

    private static IntPtr Allocate(int size, uint protect) {
        IntPtr memory = VirtualAlloc(
            IntPtr.Zero, new UIntPtr((uint)size), MemCommit | MemReserve, protect);
        if (memory == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        return memory;
    }

    private static int ReadInt32(byte[] bytes, int offset) {
        return BitConverter.ToInt32(bytes, offset);
    }

    private static int FileOffsetForVa(byte[] image, int va) {
        int pe = ReadInt32(image, 0x3C);
        int sectionCount = BitConverter.ToUInt16(image, pe + 6);
        int optionalSize = BitConverter.ToUInt16(image, pe + 20);
        int optional = pe + 24;
        int imageBase = ReadInt32(image, optional + 28);
        int sections = optional + optionalSize;
        int rva = va - imageBase;
        for (int i = 0; i < sectionCount; ++i) {
            int header = sections + i * 40;
            int virtualSize = ReadInt32(image, header + 8);
            int sectionRva = ReadInt32(image, header + 12);
            int rawSize = ReadInt32(image, header + 16);
            int rawOffset = ReadInt32(image, header + 20);
            if (rva >= sectionRva && rva < sectionRva + Math.Max(virtualSize, rawSize)) {
                return rawOffset + rva - sectionRva;
            }
        }
        throw new InvalidDataException("VA is not backed by a PE section: 0x" + va.ToString("X8"));
    }

    private static IntPtr[] InitializeAbsoluteData(byte[] image) {
        IntPtr constants = Allocate(0x20, PageReadWrite);
        int constantOffset = FileOffsetForVa(image, 0x0066FEE0);
        Marshal.Copy(image, constantOffset, constants, 0x20);

        IntPtr tableMemory = Allocate(512, PageReadWrite);
        byte[] table = new byte[512];
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                int offset = (x * 8 + y) * 8;
                table[offset + 0] = (byte)((y >> 1) + 4 * (x >> 1));
                table[offset + 1] = (byte)((y >> 1) + 4 * ((x + 1) >> 1));
                table[offset + 2] = (byte)(((y + 1) >> 1) + 4 * (x >> 1));
                table[offset + 3] = (byte)(((y + 1) >> 1) + 4 * ((x + 1) >> 1));
                table[offset + 4] = (byte)(5 * ((y & 1) + 2 * (x & 1)));
                table[offset + 5] = (byte)(5 * ((y & 1) + 2 * ((x + 1) & 1)));
                table[offset + 6] = (byte)(5 * (((y + 1) & 1) + 2 * (x & 1)));
                table[offset + 7] = (byte)(5 * (((y + 1) & 1) + 2 * ((x + 1) & 1)));
            }
        }
        Marshal.Copy(table, 0, tableMemory, table.Length);
        return new IntPtr[] { constants, tableMemory };
    }

    private static void PatchAddress(byte[] code, int oldAddress, int newAddress) {
        byte[] oldBytes = BitConverter.GetBytes(oldAddress);
        byte[] newBytes = BitConverter.GetBytes(newAddress);
        int replacements = 0;
        for (int i = 0; i <= code.Length - 4; ++i) {
            if (code[i] != oldBytes[0] || code[i + 1] != oldBytes[1] ||
                code[i + 2] != oldBytes[2] || code[i + 3] != oldBytes[3]) continue;
            Array.Copy(newBytes, 0, code, i, 4);
            ++replacements;
            i += 3;
        }
        if (replacements == 0) {
            throw new InvalidDataException("address is absent from original function: 0x" + oldAddress.ToString("X8"));
        }
    }

    private static int Quantize(float coordinate) {
        float doubled = coordinate + coordinate;
        float shifted = doubled + 1.0f;
        float below = 0.49998998641967773f - shifted;
        float encoded = 12582912.0f - below;
        FloatBits bits = new FloatBits { Value = encoded };
        return (bits.Bits & 0x7FFFFF) - 0x400001;
    }

    private static float[] Candidate(
        float[][] blocks, float originX, float originY,
        float x, float y, float z) {
        float sx = x - originX;
        float sy = y - originY;
        float sz = z - -2.5f;
        int ix = Quantize(sx);
        int iy = Quantize(sy);
        int iz = Quantize(sz);
        if (ix < 0 || ix > 4 || iy < 0 || iy > 4 || iz < 0 || iz > 3) {
            return new float[] { x, y, z };
        }

        float fx = (sx + sx) - ix;
        float fy = (sy + sy) - iy;
        float fz = (sz + sz) - iz;
        float nx = 1.0f - fx;
        float ny = 1.0f - fy;
        float nz = 1.0f - fz;
        int[] block = {
            (iy >> 1) + 4 * (ix >> 1),
            (iy >> 1) + 4 * ((ix + 1) >> 1),
            ((iy + 1) >> 1) + 4 * (ix >> 1),
            ((iy + 1) >> 1) + 4 * ((ix + 1) >> 1)
        };
        int[] local = {
            5 * ((iy & 1) + 2 * (ix & 1)) + iz,
            5 * ((iy & 1) + 2 * ((ix + 1) & 1)) + iz,
            5 * (((iy + 1) & 1) + 2 * (ix & 1)) + iz,
            5 * (((iy + 1) & 1) + 2 * ((ix + 1) & 1)) + iz
        };
        float[] result = new float[3];
        for (int channel = 0; channel < 3; ++channel) {
            float c000 = blocks[block[0]][local[0] * 3 + channel];
            float c001 = blocks[block[0]][local[0] * 3 + 3 + channel];
            float c100 = blocks[block[1]][local[1] * 3 + channel];
            float c101 = blocks[block[1]][local[1] * 3 + 3 + channel];
            float c010 = blocks[block[2]][local[2] * 3 + channel];
            float c011 = blocks[block[2]][local[2] * 3 + 3 + channel];
            float c110 = blocks[block[3]][local[3] * 3 + channel];
            float c111 = blocks[block[3]][local[3] * 3 + 3 + channel];
            result[channel] =
                c000 * (nx * ny * nz) + c001 * (nx * ny * fz) +
                c100 * (fx * ny * nz) + c101 * (fx * ny * fz) +
                c010 * (nx * fy * nz) + c011 * (nx * fy * fz) +
                c110 * (fx * fy * nz) + c111 * (fx * fy * fz);
        }
        return result;
    }

    private static float ReadFloat(IntPtr pointer, int offset) {
        FloatBits value = new FloatBits { Bits = Marshal.ReadInt32(pointer, offset) };
        return value.Value;
    }

    private static void WriteFloat(IntPtr pointer, int offset, float value) {
        FloatBits bits = new FloatBits { Value = value };
        Marshal.WriteInt32(pointer, offset, bits.Bits);
    }

    private static bool Close(float left, float right) {
        float scale = Math.Max(1.0f, Math.Max(Math.Abs(left), Math.Abs(right)));
        return Math.Abs(left - right) <= 2.0e-5f * scale;
    }

    private static int Main(string[] args) {
        if (IntPtr.Size != 4) throw new InvalidOperationException("build with /platform:x86");
        if (args.Length != 1) {
            Console.Error.WriteLine("usage: field_sampler_difftest.exe path-to-DKII-DX.exe");
            return 2;
        }
        byte[] image = File.ReadAllBytes(args[0]);
        IntPtr[] data = InitializeAbsoluteData(image);
        const int functionSize = 1095;
        byte[] code = new byte[functionSize];
        Array.Copy(image, FileOffsetForVa(image, 0x0058F030), code, 0, code.Length);
        PatchAddress(code, 0x0066FEE0, data[0].ToInt32() + 0x00);
        PatchAddress(code, 0x0066FEF0, data[0].ToInt32() + 0x10);
        PatchAddress(code, 0x0066FEF8, data[0].ToInt32() + 0x18);
        PatchAddress(code, 0x0066FEFC, data[0].ToInt32() + 0x1C);
        PatchAddress(code, 0x00780E78, data[1].ToInt32());
        IntPtr executable = Allocate(code.Length, PageExecuteReadWrite);
        Marshal.Copy(code, 0, executable, code.Length);
        OriginalSampler original = (OriginalSampler)Marshal.GetDelegateForFunctionPointer(
            executable, typeof(OriginalSampler));

        Random random = new Random(0x58F030);
        float[][] blocks = new float[16][];
        IntPtr pointerTable = Allocate(blocks.Length * 4, PageReadWrite);
        for (int block = 0; block < blocks.Length; ++block) {
            blocks[block] = new float[60];
            for (int i = 0; i < blocks[block].Length; ++i) {
                blocks[block][i] = (float)(random.NextDouble() * 16.0 - 8.0);
            }
            IntPtr blockMemory = Allocate(blocks[block].Length * 4, PageReadWrite);
            Marshal.Copy(blocks[block], 0, blockMemory, blocks[block].Length);
            Marshal.WriteInt32(pointerTable, block * 4, blockMemory.ToInt32());
        }
        IntPtr self = Allocate(12, PageReadWrite);
        Marshal.WriteInt32(self, 0, pointerTable.ToInt32());
        IntPtr output = Allocate(12, PageReadWrite);

        for (int iteration = 0; iteration < 100000; ++iteration) {
            float originX = (float)(random.NextDouble() * 4.0 - 2.0);
            float originY = (float)(random.NextDouble() * 4.0 - 2.0);
            float sx = (float)(random.NextDouble() * 3.5 - 0.5);
            float sy = (float)(random.NextDouble() * 3.5 - 0.5);
            float sz = (float)(random.NextDouble() * 3.0 - 0.5);
            float x = originX + sx;
            float y = originY + sy;
            float z = -2.5f + sz;
            WriteFloat(self, 4, originX);
            WriteFloat(self, 8, originY);
            WriteFloat(output, 0, 123.0f);
            WriteFloat(output, 4, 456.0f);
            WriteFloat(output, 8, 789.0f);
            IntPtr returned = original(self, x, y, z, output);
            if (returned != output) throw new Exception("original returned the wrong pointer");
            float[] expected = Candidate(blocks, originX, originY, x, y, z);
            for (int channel = 0; channel < 3; ++channel) {
                float actual = ReadFloat(output, channel * 4);
                if (!Close(actual, expected[channel])) {
                    Console.Error.WriteLine(
                        "mismatch at iteration {0}, channel {1}: original={2:R}, candidate={3:R}",
                        iteration, channel, actual, expected[channel]);
                    Console.Error.WriteLine(
                        "origin=({0:R},{1:R}) input=({2:R},{3:R},{4:R})",
                        originX, originY, x, y, z);
                    return 1;
                }
            }
        }
        Console.WriteLine("OK: 100000 original 0058F030 cases match trilinear semantics");
        return 0;
    }
}

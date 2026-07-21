//
// Created by DiaLight on 9/17/2025.
//

#ifndef FLAMETAL_STACKFRAME_H
#define FLAMETAL_STACKFRAME_H

#include <string>
#include <Windows.h>

struct StackFrame {

    DWORD eip = 0;
    DWORD esp = 0;
    DWORD ebp = 0;

    std::string libName;
    DWORD libBase = 0;
    std::string symName;
    DWORD symAddr = 0;

    friend std::ostream &operator<<(std::ostream &os, const StackFrame &frame);

    void reset();
};


std::ostream &operator<<(std::ostream &os, const StackFrame &frame);

std::string StackFrame_getReadableSymName(const std::string &symName);


#endif // FLAMETAL_STACKFRAME_H

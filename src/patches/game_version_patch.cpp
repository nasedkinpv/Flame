//
// Created by DiaLight on 06.08.2024.
//

#include "game_version_patch.h"
#include <Windows.h>
#include <cstdio>

bool patch::game_version_patch::enabled = true;

EXTERN_C __declspec(dllexport) char Flametal_version[64] = {'\0', '1'};

char *patch::game_version_patch::getFileVersion() {
    if(!enabled) return nullptr;
    if(Flametal_version[0] == '\0') return nullptr;
    return Flametal_version;
}

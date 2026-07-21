//
// Created by DiaLight on 18.06.2024.
//

#ifndef FLAMETAL_COFFBUILDER_H
#define FLAMETAL_COFFBUILDER_H

#include <vector>
#include "chunk/Chunk.h"

[[nodiscard]] bool buildCoff(std::vector<Chunk *> &chunks, std::vector<uint8_t> &buf);


#endif //FLAMETAL_COFFBUILDER_H

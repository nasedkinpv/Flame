//
// Created by DiaLight on 25.06.2024.
//

#ifndef FLAMETAL_CHUNKARENA_H
#define FLAMETAL_CHUNKARENA_H

#include <vector>
#include <memory>

struct Chunk;
struct ChunkRef;

struct ChunkArena {

    std::vector<std::unique_ptr<Chunk>> chunks;
    std::vector<std::unique_ptr<ChunkRef>> refs;

};


#endif //FLAMETAL_CHUNKARENA_H

# https://github.com/werkt/kwd (MIT) -- struct layouts for DK2's .kwd/.kld
# level format (Terrain/Object/Creature/Room/... blocks) plus each block's
# raw-struct -> "LoadBlock" normalizer (name, id, ArtResource texture/mesh
# references, ...).
#
# Only the format-struct headers and the portable *LoadBlock::load() bodies
# are compiled here: upstream's KWD.cpp/File.cpp file-IO layer
# (TbDiscFile/class_672fe8/...) is POSIX-only (unistd.h/dirent.h/pthread.h)
# and isn't needed for our read-only JSON dump, so tools/kwd_dump implements
# its own small MSVC-portable tag reader against the same struct layouts
# instead of porting that layer. The unused sources are still vendored
# alongside (KWD.cpp/KWD.h/File.cpp/File.h/TbString.*) for provenance/license
# fidelity with upstream.
add_library(kwd STATIC
        src/Terrain.cpp
        src/Object.cpp
        src/Room.cpp
        src/Creature.cpp
        src/KeeperSpell.cpp
        src/Effect.cpp
        src/EffectElement.cpp
        src/Thing.cpp
)
target_include_directories(kwd PUBLIC src)

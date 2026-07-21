// kwd_dump: prints Terrain/Object/Creature definitions from a DK2 .kwd/.kld
// level file as JSON, including every ArtResource (mesh/sprite/texture)
// reference each definition carries by name. This is the cross-reference
// table for TextureDump's raw named PNGs: a texture/mesh resource name dumped
// by the game (see src/dk2/TextureDump.cpp) can be looked up here against
// the semantic entity (terrain tile, object, creature) that uses it.
//
// This is a from-scratch, MSVC/host-portable re-implementation of the
// section-tag walk in vendored libs/kwd's KWD.cpp, using plain fread/fseek
// instead of that upstream's POSIX-only TbDiscFile/File.cpp layer (open()/
// pthread/dirent - not available on Windows without extra porting work we
// don't need). The vendored *Block structs and *LoadBlock::load() field
// normalizers (libs/kwd/src/Terrain.cpp, Object.cpp, Creature.cpp, ...) are
// reused as-is; only the file-format tag walk is our own code, kept
// byte-exact with KWD.cpp's logic (same section ids, header sizes, and
// per-record byte lengths) so the file offset never drifts even through
// sections we don't care about (rooms, doors, traps, things, triggers, ...).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Terrain.h"
#include "Object.h"
#include "Creature.h"

namespace {

FILE *g_file = nullptr;

bool readExact(void *buf, size_t size) {
    return g_file && fread(buf, 1, size, g_file) == size;
}

bool skip(long bytes) {
    if (bytes < 0) return false;
    return g_file && fseek(g_file, bytes, SEEK_CUR) == 0;
}

// Generic section tag: {id, size, up to 8 trailing uint32 words}. Real
// on-disk headers are 12/16/36/40 bytes of this shape (see KWD.cpp); we
// always read only as many bytes as the section needs and leave the rest
// zeroed.
struct Tag {
    uint32_t id = 0;
    uint32_t size = 0;
    uint32_t words[8] = {};
};

bool readTag(Tag &t, size_t totalBytes) {
    std::memset(&t, 0, sizeof(t));
    if (totalBytes > sizeof(t)) return false;
    return readExact(&t, totalBytes);
}

std::string jsonEscape(const char *s, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < maxLen && s[i]; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

bool g_firstEntity = true;

void beginEntity(FILE *out, const char *kind, const char *name, unsigned id) {
    if (!g_firstEntity) fprintf(out, ",\n");
    g_firstEntity = false;
    fprintf(out, "  {\n    \"kind\": \"%s\",\n    \"id\": %u,\n    \"name\": \"%s\",\n    \"resources\": [\n",
            kind, id, jsonEscape(name, 64).c_str());
}

void endEntity(FILE *out) {
    fprintf(out, "\n    ]\n  }");
}

bool g_firstResource = true;

void emitResource(FILE *out, const char *slot, const ArtResource &r) {
    if (!g_firstResource) fprintf(out, ",\n");
    g_firstResource = false;
    fprintf(out,
            "      { \"slot\": \"%s\", \"name\": \"%s\", \"type\": %u, "
            "\"flags\": %u, \"start_af\": %u, \"end_af\": %u, "
            "\"image_w\": %u, \"image_h\": %u, \"image_frames\": %d, "
            "\"mesh_scale\": %d, \"mesh_frames\": %u, "
            "\"anim_frames\": %u, \"anim_fps\": %u, "
            "\"proc_id\": %u }",
            slot, jsonEscape(r.name, 64).c_str(), r.settings.type,
            r.settings.flags, r.settings.start_af, r.settings.end_af,
            r.settings.data.image.width, r.settings.data.image.height, r.settings.data.image.frames,
            r.settings.data.mesh.scale, r.settings.data.mesh.frames,
            r.settings.data.anim.frames, r.settings.data.anim.fps,
            r.settings.data.proc.id);
}

void resetResources() { g_firstResource = true; }

// --- section handlers -------------------------------------------------
// Every homogeneous fixed-record section (rooms/traps/doors/keeper spells/
// creature spells/players/variables/effect elements/effects/shots/map) is
// skipped as a single blob: KWD.cpp's own "no callback registered" path
// already does exactly that (seek forward by the data tag's byte count),
// since we never need those sections' contents for this tool.
bool skipHomogeneousSection(uint32_t headerId, uint32_t dataId, size_t headerBytes) {
    Tag header;
    if (!readTag(header, headerBytes) || header.id != headerId) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != dataId) return false;
    return skip(static_cast<long>(data.size));
}

bool parseTerrain(FILE *out) {
    Tag header;
    if (!readTag(header, 36) || header.id != 111) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != 112) return false;
    uint32_t count = header.words[0];
    if (count == 0) return true;
    long recordLen = static_cast<long>(data.size / count);
    if (recordLen != 552) return false;  // matches KWD.cpp's own hard check
    for (uint32_t i = 0; i < count; ++i) {
        TerrainBlock block;
        if (!readExact(&block, sizeof(block))) return false;
        TerrainLoadBlock loadBlock;
        loadBlock.load(block);
        const TerrainBlock &b = loadBlock.block();
        beginEntity(out, "terrain", b.name, b.id);
        resetResources();
        emitResource(out, "complete", b.complete);
        emitResource(out, "side", b.side);
        emitResource(out, "top", b.top);
        emitResource(out, "tagged", b.tagged);
        endEntity(out);
    }
    return true;
}

bool parseObjects(FILE *out) {
    Tag header;
    if (!readTag(header, 36) || header.id != 241) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != 242) return false;
    uint32_t count = header.words[0];
    if (count == 0) return true;
    long recordLen = static_cast<long>(data.size / count);
    if (recordLen != 894) return false;
    for (uint32_t i = 0; i < count; ++i) {
        ObjectBlock block;
        if (!readExact(&block, sizeof(block))) return false;
        ObjectLoadBlock loadBlock;
        loadBlock.load(block);
        const ObjectBlock &b = loadBlock.block();
        beginEntity(out, "object", b.name, b.id);
        resetResources();
        emitResource(out, "mesh", b.kMeshResource);
        emitResource(out, "gui_icon", b.kGuiIconResource);
        emitResource(out, "in_hand_icon", b.kInHandIconResource);
        emitResource(out, "in_hand_mesh", b.kInHandMeshResource);
        emitResource(out, "unknown", b.kUnknownResource);
        for (int j = 0; j < 4; ++j) {
            char slot[32];
            snprintf(slot, sizeof(slot), "additional%d", j);
            emitResource(out, slot, b.kAdditionalResources[j]);
        }
        endEntity(out);
    }
    return true;
}

bool parseCreatures(FILE *out) {
    Tag header;
    if (!readTag(header, 36) || header.id != 171) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != 172) return false;
    uint32_t count = header.words[0];
    if (count == 0) return true;
    long recordLen = static_cast<long>(data.size / count);
    if (recordLen != 5449) return false;
    for (uint32_t i = 0; i < count; ++i) {
        CreatureBlock block;
        if (!readExact(&block, sizeof(block))) return false;
        CreatureLoadBlock loadBlock;
        loadBlock.load(block);
        beginEntity(out, "creature", loadBlock.name(), loadBlock.id());
        resetResources();
        for (int j = 0; j < 39; ++j) {
            char slot[32];
            snprintf(slot, sizeof(slot), "anim%d", j);
            emitResource(out, slot, loadBlock.ref1(j));
        }
        emitResource(out, "ref2", loadBlock.ref2());
        emitResource(out, "reff77", loadBlock.reff77());
        emitResource(out, "ref3", loadBlock.ref3());
        emitResource(out, "ref4", loadBlock.ref4());
        for (int j = 0; j < 6; ++j) {
            char slot[32];
            snprintf(slot, sizeof(slot), "ref5_%d", j);
            emitResource(out, slot, loadBlock.ref5(j));
        }
        emitResource(out, "ref6", loadBlock.ref6());
        for (int j = 0; j < 3; ++j) {
            char slot[32];
            snprintf(slot, sizeof(slot), "ref7_%d", j);
            emitResource(out, slot, loadBlock.ref7(j));
        }
        emitResource(out, "ref8", loadBlock.ref8());
        endEntity(out);
    }
    return true;
}

// Things (190): unlike the other sections there is no single "skip the
// whole blob" byte count -- KWD.cpp always walks record-by-record. We don't
// register any per-type callback (we don't need Things data), so every
// case there resolves to "seek forward by this record's byte length"; a
// handful of tag ids (193/202/206) have no known length and KWD.cpp
// aborts on them -- we instead stop the section cleanly and report it,
// since a corrupt/unsupported file should not crash this tool (see
// TextureDump.cpp's own "skip on anomaly" contract for the same principle
// applied on the game side).
bool parseThingsSkip(FILE *stderrOut) {
    Tag header;
    if (!readTag(header, 36) || header.id != 191) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != 192) return false;
    uint32_t count = header.words[0];
    for (uint32_t i = 0; i < count; ++i) {
        Tag thingTag;
        if (!readTag(thingTag, 8)) return false;
        switch (thingTag.id) {
        case 193:
        case 202:
        case 206:
            fprintf(stderrOut, "kwd_dump: unsupported Things tag %u, stopping Things section early\n",
                    thingTag.id);
            return false;
        default:
            if (!skip(static_cast<long>(thingTag.size))) return false;
        }
    }
    return true;
}

// Triggers (210): also record-by-record; ids 213/214 carry a size we can
// skip, anything else is left alone (matches KWD.cpp, which does not
// consume extra bytes for unrecognized ids either).
bool parseTriggersSkip() {
    Tag header;
    if (!readTag(header, 40) || header.id != 211) return false;
    Tag data;
    if (!readTag(data, 8) || data.id != 212) return false;
    uint32_t count = header.words[0] + header.words[1];
    for (uint32_t i = 0; i < count; ++i) {
        Tag triggerTag;
        if (!readTag(triggerTag, 8)) return false;
        if (triggerTag.id == 213 || triggerTag.id == 214) {
            if (!skip(static_cast<long>(triggerTag.size))) return false;
        }
    }
    return true;
}

// Level (220): irregular fixed structure (see KWD.cpp sub_56c4e0). We only
// need to consume the exact same bytes so later sections don't desync.
bool parseLevelSkip() {
    Tag header;
    if (!readTag(header, 36) || header.id != 221) return false;
    Tag levelAfterDd;
    if (!readTag(levelAfterDd, 8) || levelAfterDd.id != 223) return false;
    if (levelAfterDd.size != 0x6453) return false;
    std::string infoBlock(0x6453, '\0');
    if (!readExact(infoBlock.data(), infoBlock.size())) return false;
    Tag type2Tag;
    if (!readTag(type2Tag, 8) || type2Tag.id != 222) return false;
    // header.words[0] packs {w08 (low16), w0a (high16)} per KWD.h's union:
    // w08 is the Type2 (per-room-info?) record count skipped here in one
    // go, w0a is the Type3 count consumed below.
    uint16_t w08 = static_cast<uint16_t>(header.words[0] & 0xFFFF);
    uint16_t w0a = static_cast<uint16_t>(header.words[0] >> 16);
    if (!skip(static_cast<long>(w08) * 72)) return false;
    for (uint16_t i = 0; i < w0a; ++i) {
        int32_t discard;
        if (!readExact(&discard, sizeof(discard))) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: kwd_dump <file.kwd|.kld>\n");
        return 1;
    }
    g_file = fopen(argv[1], "rb");
    if (!g_file) {
        fprintf(stderr, "kwd_dump: failed to open \"%s\"\n", argv[1]);
        return 1;
    }

    printf("[\n");
    bool ok = true;
    while (ok) {
        uint32_t outer[3];
        size_t got = fread(outer, 1, sizeof(outer), g_file);
        if (got == 0 && feof(g_file)) break;  // clean EOF between sections
        if (got != sizeof(outer)) { ok = false; break; }
        if (outer[0] - 100 > 170) { ok = false; break; }  // same sanity check as KWD.cpp
        switch (outer[0]) {
        case 100: ok = skipHomogeneousSection(101, 102, 16); break;
        case 110: ok = parseTerrain(stdout); break;
        case 120: ok = skipHomogeneousSection(121, 122, 36); break;
        case 130: ok = skipHomogeneousSection(131, 132, 36); break;
        case 140: ok = skipHomogeneousSection(141, 142, 36); break;
        case 150: ok = skipHomogeneousSection(151, 152, 36); break;
        case 160: ok = skipHomogeneousSection(161, 162, 36); break;
        case 170: ok = parseCreatures(stdout); break;
        case 180: ok = skipHomogeneousSection(181, 182, 36); break;
        case 190: ok = parseThingsSkip(stderr); break;
        case 210: ok = parseTriggersSkip(); break;
        case 220: ok = parseLevelSkip(); break;
        case 230: ok = skipHomogeneousSection(231, 232, 36); break;
        case 240: ok = parseObjects(stdout); break;
        case 250: ok = skipHomogeneousSection(251, 252, 36); break;
        case 260: ok = skipHomogeneousSection(261, 262, 36); break;
        case 270: ok = skipHomogeneousSection(271, 272, 36); break;
        default:
            fprintf(stderr, "kwd_dump: unknown section id %u, stopping\n", outer[0]);
            ok = false;
            break;
        }
    }
    fclose(g_file);
    printf("\n]\n");

    if (!ok) {
        fprintf(stderr, "kwd_dump: stopped early (possibly incomplete output above)\n");
        return 2;
    }
    return 0;
}

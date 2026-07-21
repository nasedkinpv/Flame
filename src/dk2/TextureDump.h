//
// Created by Flametal contributor.
//
// Named texture dump: writes every uniquely-named decoded engine surface
// (MySurface, already-decompressed pixels) to disk as a PNG file named after
// its resource name/id, before it gets composited into an atlas page. See
// TextureDump.cpp for the flame option and hook contract.
#ifndef FLAMETAL_DK2_TEXTURE_DUMP_H
#define FLAMETAL_DK2_TEXTURE_DUMP_H

namespace dk2 {
struct MySurface;
}

namespace patch::texture_dump {

// Cheap to call unconditionally from the decode/paint hot path: the option
// is checked first and the whole call is a no-op (single string compare)
// when `flametal:TextureDump` is unset. `name` is the resource name recorded
// in MyStringHashMap_MyCESurfHandle (e.g. the WAD entry / texture id string);
// `surf` is the decoded-but-not-yet-composited MySurface. Each unique name is
// written at most once per process run.
void onDecodedSurface(const char *name, const dk2::MySurface *surf);

}  // namespace patch::texture_dump

#endif  // FLAMETAL_DK2_TEXTURE_DUMP_H

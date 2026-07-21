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

// World/terrain/creature/room textures are backed by CEngineCompressedSurface
// (see MyTextures::loadCompressed / EngineTextures.dat), which decompresses
// straight into the destination page's locked buffer inside
// CEngineCompressedSurface::copySurf -- there is no separate decoded MySurface
// to hand a name to, because the compressed surface object itself carries no
// resource name (only the MyCESurfHandle that resolved it does). Callers that
// are about to composite a handle with a known name (MyCESurfHandle::paint,
// SurfHashList::expandPut) call setCompositeSourceName() immediately before
// invoking paintSurf()/copySurf() on it, and clear it (pass nullptr)
// immediately after. copySurf() then calls onCompositedSurfaceDecoded() with
// the freshly-decoded rect, which looks the name up via this context.
// Not thread-safe, but the paint/composite path is single-threaded.
void setCompositeSourceName(const char *name);

// See setCompositeSourceName(). No-op (including the context lookup) unless
// flametal:TextureDump is set.
void onCompositedSurfaceDecoded(const dk2::MySurface *surf);

}  // namespace patch::texture_dump

#endif  // FLAMETAL_DK2_TEXTURE_DUMP_H

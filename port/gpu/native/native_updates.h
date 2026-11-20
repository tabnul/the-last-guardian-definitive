// SPDX-License-Identifier: GPL-2.0-or-later
// What is left of the writer announcements (N5a .. B9, the old PS4 path's update audit): their modes as constants and the B8 counters other modules still count.
// The announcements themselves (NoteWriterRange, the Map / Unmap windows, the batches and their application) were deleted on 2026-11-20 (backend note 102):
// nothing consumed them (no renderer registers an apply), and with the engine-to-Vulkan backend every announcement returned at once.

#pragma once

#include <array>
#include <atomic>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

namespace Core::NativeUpdates {

[[nodiscard]] constexpr u32 B2NativeMode() noexcept {
    return 2u; // native, no switch since the cleanup of 2026-10-30
}
/// `object` value meaning "engine buffers only, no single id" (a block over several buffers)
constexpr u64 kBuffersOnly = ~u64{0};
[[nodiscard]] constexpr u32 B8BufferImagesMode() noexcept {
    return 2u;
}
[[nodiscard]] constexpr u32 B9LifetimeMode() noexcept {
    return 2u;
}
[[nodiscard]] constexpr bool B9PlaceOn() noexcept {
    return true;
}
enum B8Count : u32 {
    B8AnnouncedOnImage,         ///< announcements that touched an image
    B8KeptForGpu,               ///< second applications that left an image the GPU wrote since alone
    B8TexturesDeleted,          ///< B8.2: engine texture deletions carried out
    B8TextureAliasKept,         ///< ... skipped: another live engine texture has the same memory
    B8DeletedImages,            ///< images of a deleted texture (same memory, not bigger than the object)
    B8DeletedImagesBigger,      ///< images at a deleted texture's memory but bigger than the object (left alone)
    B8DeletedImagesFreed,       ///< native mode: images freed at the engine's deletion
    B8UsedAfterDeleteReused,    ///< audit: an image of a deleted texture used again while a new engine texture owns its memory (native mode makes a new image)
    B8UsedAfterDeleteNoOwner,   ///< audit: ... while no engine texture owns its memory (native mode would lose it: must stay 0)
    B8ViewIdsDropped,           ///< B3 entries of deleted views dropped
    B8FindById,                 ///< B8.3: image lookups answered by the engine texture's id
    B8FindByIdOther,            ///< ... audit: the lookup by address found another image
    B8FindNoImageYet,           ///< ... the texture has no matching image yet (page table lookup, then owned)
    B8FindNoOwner,              ///< ... no engine texture starts at the memory (page table lookup)
    B8FindOwned,                ///< images given to an engine texture
    B8FindByAnswer,             ///< B8.4: lookups answered by the engine texture's remembered answer (a view of another format or size, a mip or layer of a bigger image)
    B8FindAnswerOther,          ///< ... audit: the lookup by address gave another image, mip or layer
    B8FindAnswerStale,          ///< ... an image was registered or removed in the range since: the page table lookup runs again
    B8FirstNoOverlap,           ///< B8.4 census: a page table lookup for an engine texture that met no image in its memory
    B8FirstOverlap,             ///< ... that met images (overlap resolution ran)
    B8RouteAnnounce,            ///< B8.4: announcements whose images were found through the engine textures of the range
    B8RouteAnnounceFallback,    ///< ... the range is not covered by engine textures (page table)
    B8RouteMissed,              ///< ... audit: an image the page table finds and the engine textures' lists do not (not looked up since: refreshed when adopted)
    B8RouteMissedListed,        ///< ... of those, images a lookup already gave out (a gap in the lists: must stay 0)
    B8Adopted,                  ///< B8.4: an older image a page table lookup gave out for the first time (native mode: refreshed from memory, its announcements may have been missed)
    B8AdoptedGpu,               ///< ... the GPU wrote it: kept
    B8CreatedTextures,          ///< B8.4: engine texture creations carried out
    B8FirstByEngine,            ///< B8.4 step 3: a first lookup whose candidate images came from the engine textures covering the memory
    B8FirstFallback,            ///< ... the memory is not covered by engine textures (page table)
    B8FirstMissedListed,        ///< ... audit: a candidate the page table finds, a lookup gave out, and the engine textures' lists lack (must stay 0)
    B8FirstStaleLeft,           ///< ... audit: an older image no lookup gave out since (native mode leaves it out: the new image does not reuse a stale one)
    B8CreatedUnlisted,          ///< ... older images in the new texture's memory, unlisted (refreshed when a lookup gives them out again)
    B8RouteCopy,                ///< B8.4: copies whose image was found through the engine texture at the address
    B8RouteCopyFallback,        ///< ... no engine texture starts at the address (page table)
    B8RouteCopyOther,           ///< ... audit: the page table gives another answer
    B8BufAnnounce,              ///< B8.5: announcements over engine buffer memory whose images came from the buffers' lists
    B8BufCopy,                  ///< ... copies at engine buffer memory, same
    B8BufListed,                ///< ... images listed by an engine buffer (at registration, or at the buffer's creation)
    B8BufMissed,                ///< ... audit: an image the page table finds that the buffers' lists lack (must stay 0)
    B8BufNotCovered,            ///< ... ranges neither textures nor buffers cover whole (page table)
    E3FindUnnamed,              ///< E3 census: FindImage calls whose description names no engine object (targets, video out, copies, internal)
    E3FindNamed,                ///< ... that name an engine resource / view (a sampled or stored texture of a record binding)
    E3NamedSameOwner,           ///< ... named: the texture starting at the memory is the named resource (address and id agree)
    E3NamedOtherOwner,          ///< ... named: another texture starts at the memory (the id and the address disagree)
    E3NamedInterior,            ///< ... named: no texture starts at the memory but the named resource contains it (a view of a part: mip / slice offset)
    E3NamedNoOwner,             ///< ... named: no engine texture holds the memory at all
    E3NamedUnknownObject,       ///< ... named: the id is not in the engine table any more
    E3OwnerById,                ///< E3.2: the engine texture of a lookup came from the named id (not the address)
    E3OwnerByIdMissed,          ///< ... the id could not be resolved (the address asked instead)
    B8Counts,
};
void NoteB8(B8Count what) noexcept;
/// The B8 counters (debug server `/updates`)
nlohmann::json Report();

} // namespace Core::NativeUpdates

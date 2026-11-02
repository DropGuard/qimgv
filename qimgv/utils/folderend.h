#pragma once

#include "settings.h"

// What a folder-boundary keypress means, given the configured FolderEndAction.
// Pure: it takes the setting and the position navigation stepped to, and says
// what should happen. Core owns doing it (loading an image, switching folder,
// showing a message), so the behavior can be tested for every configured value
// instead of only the one this machine happens to have set.
namespace FolderEnd {

enum class Outcome {
    LoadIndex,            // an image in this folder, at `index`
    EnterAdjacentFolder,  // leave for the next / previous sibling folder
    ReportBoundary        // nothing configured: there is nothing beyond this edge
};

struct Decision {
    Outcome outcome;
    int index = -1; // only meaningful for LoadIndex
};

// `index` is already stepped (+1 or -1) and may be out of range; `count` is the
// number of files in the folder. Which edge was hit is the caller's business —
// nextImage() and prevImage() already know, and it only picks the message.
Decision decide(FolderEndAction action, int index, unsigned long count);

} // namespace FolderEnd

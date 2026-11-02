#include "folderend.h"

namespace FolderEnd {

Decision decide(FolderEndAction action, int index, unsigned long count) {
    if(index >= 0 && index < static_cast<int>(count))
        return {Outcome::LoadIndex, index};

    // Out of range: this is a folder boundary, and the setting decides what one
    // means. An empty folder lands here too — its only index is out of range.
    switch(action) {
    case FOLDER_END_LOOP:
        // wrap to the far end of the same folder; the sign says which edge was hit
        return {Outcome::LoadIndex, index < 0 ? static_cast<int>(count) - 1 : 0};
    case FOLDER_END_GOTO_ADJACENT:
        return {Outcome::EnterAdjacentFolder, -1};
    case FOLDER_END_NO_ACTION:
        break;
    }
    return {Outcome::ReportBoundary, -1};
}

} // namespace FolderEnd

#pragma once

// The answer vocabulary of the file-replace question, shared by the dialog
// that asks it, the interface that abstracts it (iuserinteractions.h), and
// the Core state machines that consume it.
//
// The conversions are load-bearing and deliberate: `operator bool()` is `yes`,
// which is how a DialogResult silently becomes the `force` argument of
// FileOperations::copyFileTo / moveFileTo — "yes" means overwrite this file,
// `all` makes the answer stick for the rest of the batch (for yes AND for no:
// no-to-all is skip-the-rest), `cancel` unwinds the whole operation.

struct DialogResult {
    bool yes = false;
    bool all = false;
    bool cancel = false;
    bool operator==(bool const &cmp) const {
        return yes == cmp;
    }
    operator bool() {
        return yes;
    }
};

enum FileReplaceMode {
    FILE_TO_FILE,
    DIR_TO_DIR,
    FILE_TO_DIR,
    DIR_TO_FILE
};

#pragma once

#include <QString>
#include "gui/dialogs/dialogresult.h"

// The questions the command surface asks the user, abstracted so tests can
// script the answers and drive the interactive state machines (doInteractiveCopy /
// doInteractiveMove and the confirmation-gated slots) without modal dialogs.
// Production implements it on MW (the dialogs belong to the main window);
// tests implement it with a scripted fake.
class IUserInteractions {
public:
    virtual ~IUserInteractions() = default;

    // yes/no question; false means the caller aborts the gated action
    virtual bool showConfirmation(const QString &title, const QString &msg) = 0;
    // the file-replace question; the DialogResult is consumed with its
    // sticky-batch semantics (see dialogresult.h)
    virtual DialogResult fileReplaceDialog(const QString &src, const QString &dst,
                                           FileReplaceMode mode, bool multiple) = 0;
};

#pragma once

// Shared fixtures for the C++ test binaries. These exist because the two
// singletons tests always fight are Settings (a process-wide config backed by
// QSettings) and, previously, the EngineBridge — the bridge became
// constructible per-test, and these helpers cover the rest.

#include <QTemporaryDir>
#include <QCoreApplication>

#include "settings.h"

// Redirects HOME and the QSettings identity into a throwaway directory for
// the lifetime of the object. Construct it BEFORE anything touches Settings —
// QStandardPaths and QSettings both resolve through these values lazily, so
// the earlier the better.
class IsolatedHome {
public:
    IsolatedHome() {
        dir.setAutoRemove(true);
        Q_ASSERT(dir.isValid());
        qputenv("HOME", dir.path().toUtf8());
        QCoreApplication::setOrganizationName("qimgv");
        QCoreApplication::setApplicationName("qimgv");
    }

    QString path() const { return dir.path(); }

private:
    QTemporaryDir dir;
};

// Swaps the global `settings` pointer to a brand-new Settings instance bound
// to a unique config identity, so a test's writes are invisible to every
// other test and to the original instance. The original pointer (and its
// live state) is restored on destruction.
class FreshSettings {
public:
    FreshSettings() {
        saved = ::settings;
        // a unique application name means a config file nothing else reads or
        // has cached: the fresh instance starts from defaults, deterministically
        const QString fresh = QStringLiteral("qimgv-test-%1-%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(++counter, 0, 16);
        QCoreApplication::setApplicationName(fresh);
        ::settings = new Settings();
    }

    ~FreshSettings() {
        delete ::settings;
        ::settings = saved;
    }

    FreshSettings(const FreshSettings &) = delete;
    FreshSettings &operator=(const FreshSettings &) = delete;

private:
    inline static int counter = 0;
    Settings *saved = nullptr;
};

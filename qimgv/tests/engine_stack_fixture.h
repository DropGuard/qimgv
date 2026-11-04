// Engine stack integration tests. This is the layer that has no Rust-side
// coverage (it is Qt event-loop behavior) and no other test -- it exists because
// bridge debugging by printf/strace was the slowest iteration loop in the project.
//
// Ownership rule: business semantics live in Rust tests; this suite owns the
// Qt-side integration -- async delivery through the event loop, signal ordering,
// rescan convergence.
//
// One class per domain, one binary. Each class gets its own fixture subdirectory
// because the engine holds inotify watches on those paths for the whole process:
// a class that deleted another class's directory would be deleting a live watch,
// which is how the listing used to die mid-scan with EBADF.
#pragma once

#include <QTest>
#include <QtEndian>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QImageIOHandler>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include "components/directorymanager/directorymanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/scaler/scaler.h"
#include "components/scaler/scalerrequest.h"
#include "components/directorymodel.h"
#include "components/cache/cache.h"
#include "components/thumbnailer/thumbnailer.h"
#include "sourcecontainers/documentinfo.h"
#include "sourcecontainers/image.h"
#include "utils/folderend.h"
#include "utils/imagelib.h"
#include "utils/filelog.h"
#include "utils/imagefactory.h"
#include "utils/randomizer.h"
#include "testhelpers.h"
#include "qimgv_engine.h"

class EngineStackFixture {
public:
    explicit EngineStackFixture(const QString &domain)
        // the pid is in the path because two instances of this binary can run at
        // once (a manual run beside a ctest run), and a shared tree means one
        // process deletes directories the other is still watching
        : fixtureBase(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                      + "/qimgv-engine-stack-test/pid" + QString::number(QCoreApplication::applicationPid())
                      + "/" + domain) {}

    void makeFile(const QString &name) {
        QFile f(fixtureDir + "/" + name);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    // each slot gets its own directory: the engine holds inotify watches on these
    // paths for the whole process, so a watched dir must never be deleted and
    // recreated behind its back
    void resetFixture() {
        ++slotSeq;
        fixtureDir = fixtureBase + QString("/slot%1").arg(slotSeq);
        QVERIFY(QDir().mkpath(fixtureDir));
        makeFile("img1.jpg");
        makeFile("img2.jpg");
        makeFile("img10.jpg");
        makeFile("skip.txt");
        QVERIFY(QDir(fixtureDir).mkdir("subdir"));
    }

    // the load environment for factory calls: tests run on their own thread,
    // so capturing from settings here is the legal side of R4's line
    LoadPolicy loadPolicy() const { return LoadPolicy::fromSettings(); }

    QSize offscreenScreenPixels() {
        QScreen *screen = QApplication::primaryScreen();
        Q_ASSERT(screen != nullptr); // the offscreen platform always provides one
        return screen->size() * screen->devicePixelRatio();
    }

    bool writeBigImage(const QString &path, const QString &format, const QSize &size) {
        QImage img(size, QImage::Format_RGB32);
        QPainter p(&img);
        QLinearGradient g(0, 0, size.width(), size.height());
        g.setColorAt(0.0, Qt::red);
        g.setColorAt(1.0, Qt::blue);
        p.fillRect(img.rect(), g);
        p.end();
        return img.save(path, format.toStdString().c_str());
    }

    std::shared_ptr<Image> spyImage(const QSignalSpy &spy, int row) {
        return spy.at(row).at(0).value<std::shared_ptr<Image>>();
    }

    // Open and wait until the listing the bridge reports belongs to THIS
    // directory. Waiting on a count cannot do that: the engine keeps serving the
    // previous listing until its scan swaps the new one in, so a count can be
    // satisfied by another test's directory — which is how file names from a
    // previous slot turned up in a round-trip assertion, passing whenever the
    // machine happened to be fast enough to finish the scan first.
    void openListed(EngineBridge &bridge, const QString &dir, int sortMode = 0, bool hidden = false) {
        QSignalSpy listed(&bridge, &EngineBridge::dirListed);
        QVERIFY(bridge.openDir(dir, sortMode, hidden));
        if(bridge.listedDirectory() != dir)
            QVERIFY(listed.wait(10000));
        QCOMPARE(bridge.listedDirectory(), dir);
    }

    // "and nothing else arrives" needs a bound, not a sleep: the loader keeps a
    // path in its task table until the reply has been delivered on this thread,
    // so an idle loader means no queued loadFinished is still on its way. The
    // sleeps this replaces (600ms "a second tier would have arrived by now")
    // passed by luck whenever the machine was fast and failed by luck otherwise.
    void waitLoaderIdle(DirectoryModel &model) {
        QTRY_VERIFY_WITH_TIMEOUT(!model.loaderBusy(), 10000);
    }

    // Serialize a listing exactly the way model.rs::write_listing does: u32 file
    // count, u32 dir count, then records (files first) of u64 handle, u64 size,
    // i64 mtime_ms, u32 name_len, u32 path_len, name bytes, path bytes, all
    // little-endian. A test needs a producer to go with EngineBridge::parseListing
    // because the states worth pinning are ones a real filesystem does not hand
    // over on demand — a snapshot that lags a file which is still on disk, for
    // instance. The handle is a placeholder: the parser skips it.
    static QByteArray arenaFor(const QStringList &files, const QStringList &dirs) {
        QByteArray arena;
        auto append = [&arena](const void *data, int n) {
            arena.append(static_cast<const char *>(data), n);
        };
        auto put32 = [&append](quint32 v) { v = qToLittleEndian(v); append(&v, 4); };
        auto put64 = [&append](quint64 v) { v = qToLittleEndian(v); append(&v, 8); };
        auto putI64 = [&append](qint64 v) { v = qToLittleEndian(v); append(&v, 8); };

        put32(quint32(files.size()));
        put32(quint32(dirs.size()));
        quint64 handle = 1;
        for(const QStringList *list : {&files, &dirs})
            for(const QString &path : *list) {
                const QByteArray name = QFileInfo(path).fileName().toUtf8();
                const QByteArray full = path.toUtf8();
                put64(handle++);
                put64(1);
                putI64(0);
                put32(quint32(name.size()));
                put32(quint32(full.size()));
                append(name.constData(), int(name.size()));
                append(full.constData(), int(full.size()));
            }
        return arena;
    }

    void init() {
        QVERIFY(home.path() != nullptr);
        // IsolatedHome already redirected HOME and the config identity
        QVERIFY(Settings::getInstance() != nullptr);
        // Upstream registers these two, and a probe says the suite passes without
        // them: in Qt 6 a pointer-to-member connection carries its argument types
        // from the declaration, so qRegisterMetaType is only load-bearing for a
        // payload that has to live in a QVariant (string-based SIGNAL/SLOT connect,
        // or QVariant conversion). Kept because removing someone else's safety net
        // on an unfinished theory is not a win — the point is not to add more.
        qRegisterMetaType<std::shared_ptr<Image>>("std::shared_ptr<Image>");
        qRegisterMetaType<ScalerRequest>("ScalerRequest");
        // no removeRecursively here: the engine keeps inotify watches on these
        // paths for the life of the process, and deleting a watched directory out
        // from under it is how a later listing dies with EBADF. main() sweeps
        // this process's own tree, before any fixture has opened anything.
        QVERIFY(QDir().mkpath(fixtureBase));
    }

protected:
    IsolatedHome home;
    QString fixtureBase;
    QString fixtureDir;
    int slotSeq = 0;
};

// qExec stops a class with "Function not found" when asked for a test function it
// does not have, which would make `engine_stack_tests someSlot` report a failure in
// every other class. A positional filter therefore goes only to the classes that own
// it — and the lookup scans the method table, because a private slot is not where
// indexOfSlot looks.
inline bool owns(const QObject &test, const QByteArray &name) {
    const QMetaObject *mo = test.metaObject();
    for(int i = 0; i < mo->methodCount(); ++i)
        if(mo->method(i).name() == name)
            return true;
    return false;
}

inline int runIfOwned(QObject &test, int argc, char **argv) {
    for(int i = 1; i < argc; ++i)
        if(argv[i][0] != '-' && !owns(test, argv[i]))
            return 0;
    return QTest::qExec(&test, argc, argv);
}

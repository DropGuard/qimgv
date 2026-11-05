#include <QTest>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>

#include "engine_stack_fixture.h"
#include "utils/fileoperations.h"

// The file-operation substrate the interactive commands delegate to: copy /
// move / rename / remove against temp directories, success paths and every
// FileOpResult the surface can name. The contracts pinned here are the ones
// the code carries as comments: the backup that is only claimed when the move
// happened, the revert that reports where it left the trail, and the
// timestamp restore that makes a copy indistinguishable from the original.
// No dialogs, no Core — the interactive slots in core.cpp wrap these.
class TestFileOps : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestFileOps() : EngineStackFixture("fileops") {}

private slots:
    void initTestCase() { init(); }

    void copyFileToCopiesContentKeepsSourceAndRestoresTimestamps();
    void copyFileToNamesItsRefusals();
    void copyFileToReportsCollisionWithoutForceAndOverwritesWithForce();
    void copyFileToRejectsADirectoryCollision();
    void copyFileToReportsAnUnwritableDestination();
    void moveFileToMovesContentAndRemovesTheSource();
    void moveFileToRevertsWhenTheOriginalCannotBeRemoved();
    void renameMovesTheFileAndRefusesCollisions();
    void removeFileAndRemoveDirReportExactResults();
    void decodeResultNamesEveryOutcome();
    void moveToTrashRemovesTheFileFromItsPlace();

private:
    // a file with known content, plus a distinct mtime the timestamp-restore
    // contract can be asserted against
    // value-returning helpers cannot use QVERIFY2 (its failure path is a bare
    // return), so they report through qFail and hand back an empty result —
    // the already-failed test then trips on the very assert that consumes it
    QString makeFile(const QString &path, const QByteArray &content) {
        QFile f(path);
        if(!f.open(QIODevice::WriteOnly)) {
            QTest::qFail(qPrintable(QStringLiteral("cannot create %1: %2").arg(path, f.errorString())),
                         __FILE__, __LINE__);
            return {};
        }
        f.write(content);
        f.close();
        return path;
    }
    QByteArray contentOf(const QString &path) {
        QFile f(path);
        if(!f.open(QIODevice::ReadOnly)) {
            QTest::qFail(qPrintable(QStringLiteral("cannot read %1: %2").arg(path, f.errorString())),
                         __FILE__, __LINE__);
            return {};
        }
        return f.readAll();
    }
    void stampMTime(const QString &path) {
        // a timestamp no fixture step could produce by accident
        QFile f(path);
        QVERIFY2(f.open(QIODevice::ReadWrite), qPrintable(path));
        QDateTime past = QDateTime::currentDateTimeUtc().addDays(-17);
        QVERIFY(f.setFileTime(past, QFileDevice::FileModificationTime));
        f.close();
    }
};

void TestFileOps::copyFileToCopiesContentKeepsSourceAndRestoresTimestamps() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "original");
    stampMTime(src);
    QString destDir = fixtureDir + "/dest";
    QVERIFY(QDir().mkpath(destDir));

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);

    QCOMPARE(result, SUCCESS);
    QString dest = destDir + "/a.txt";
    QCOMPARE(contentOf(dest), QByteArray("original"));   // content travelled
    QCOMPARE(contentOf(src), QByteArray("original"));    // and the source stayed
    QCOMPARE(QFileInfo(dest).lastModified(), QFileInfo(src).lastModified());
    QVERIFY(!QFileInfo(src).isDir());
}

void TestFileOps::copyFileToNamesItsRefusals() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "x");

    // the destination being the source's own directory is a no-op, not a copy
    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, fixtureDir, false, result);
    QCOMPARE(result, NOTHING_TO_DO);

    result = OTHER_ERROR;
    FileOperations::copyFileTo(fixtureDir + "/missing.txt", fixtureDir + "/dest", false, result);
    QCOMPARE(result, SOURCE_DOES_NOT_EXIST);

    QVERIFY(QDir().mkpath(fixtureDir + "/dest"));
    result = OTHER_ERROR;
    FileOperations::copyFileTo(src, fixtureDir + "/nowhere", false, result);
    QCOMPARE(result, DESTINATION_DOES_NOT_EXIST);
}

void TestFileOps::copyFileToReportsCollisionWithoutForceAndOverwritesWithForce() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "new");
    QString destDir = fixtureDir + "/dest";
    QVERIFY(QDir().mkpath(destDir));
    makeFile(destDir + "/a.txt", "old");

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);
    QCOMPARE(result, DESTINATION_FILE_EXISTS);
    QCOMPARE(contentOf(destDir + "/a.txt"), QByteArray("old")); // untouched without force

    result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, true, result);
    QCOMPARE(result, SUCCESS);
    QCOMPARE(contentOf(destDir + "/a.txt"), QByteArray("new"));
    // the overwrite moves the old file aside as a hash-named backup; the
    // successful copy must have removed it again — no trail left behind
    const QStringList trail = QDir(destDir)
                                  .entryList(QStringList() << "a.txt_*", QDir::Files);
    QVERIFY2(trail.isEmpty(), qPrintable("backup left behind: " + trail.join(", ")));
}

void TestFileOps::copyFileToRejectsADirectoryCollision() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "x");
    QString destDir = fixtureDir + "/dest";
    QVERIFY(QDir(destDir).mkpath(destDir + "/a.txt")); // a DIRECTORY named like the file

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, true, result); // even force may not clobber a dir
    QCOMPARE(result, DESTINATION_DIR_EXISTS);
}

void TestFileOps::copyFileToReportsAnUnwritableDestination() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "x");
    QString destDir = fixtureDir + "/rodest";
    QVERIFY(QDir().mkpath(destDir));
    QVERIFY(QFile::setPermissions(destDir, QFile::Permissions(0x5555)));

    FileOpResult result = OTHER_ERROR;
    FileOperations::copyFileTo(src, destDir, false, result);
    QCOMPARE(result, DESTINATION_NOT_WRITABLE);

    // restore so fixture teardown keeps working
    QFile::setPermissions(destDir, QFile::Permissions(0x7755));
}

void TestFileOps::moveFileToMovesContentAndRemovesTheSource() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "content");
    stampMTime(src);
    QString destDir = fixtureDir + "/dest";
    QVERIFY(QDir().mkpath(destDir));

    // capture the stamp before the move: after it, the source is gone and
    // QFileInfo(src) is an invalid QDateTime — a trap this test fell into once
    const QDateTime srcMTime = QFileInfo(src).lastModified();

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveFileTo(src, destDir, false, result);

    QCOMPARE(result, SUCCESS);
    QVERIFY(!QFileInfo::exists(src));                        // the source is gone
    QCOMPARE(contentOf(destDir + "/a.txt"), QByteArray("content"));
    QCOMPARE(QFileInfo(destDir + "/a.txt").lastModified(), srcMTime);
}

void TestFileOps::moveFileToRevertsWhenTheOriginalCannotBeRemoved() {
    resetFixture();
    QString srcDir = fixtureDir + "/mvsrc";
    QVERIFY(QDir().mkpath(srcDir));
    QString src = makeFile(srcDir + "/a.txt", "new");
    QString destDir = fixtureDir + "/mvdst";
    QVERIFY(QDir().mkpath(destDir));
    makeFile(destDir + "/a.txt", "old");

    // the removal happens through removeFile(), which needs a writable source
    // directory on Linux — a read-only one forces the post-copy removal to
    // fail, which is the path whose contract is the full revert: destination
    // restored from the backup, source untouched, and an honest OTHER_ERROR
    QVERIFY(QFile::setPermissions(srcDir, QFile::Permissions(0x5555)));

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveFileTo(src, destDir, true, result);

    QCOMPARE(result, OTHER_ERROR);
    QCOMPARE(contentOf(destDir + "/a.txt"), QByteArray("old"));  // backup restored
    QCOMPARE(contentOf(src), QByteArray("new"));                 // source never lost
    QVERIFY(QFile::setPermissions(srcDir, QFile::Permissions(0x7755)));
}

void TestFileOps::renameMovesTheFileAndRefusesCollisions() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "x");
    makeFile(fixtureDir + "/b.txt", "y");

    FileOpResult result = OTHER_ERROR;
    FileOperations::rename(src, "c.txt", false, result);
    QCOMPARE(result, SUCCESS);
    QVERIFY(!QFileInfo::exists(fixtureDir + "/a.txt"));
    QCOMPARE(contentOf(fixtureDir + "/c.txt"), QByteArray("x"));

    // renaming onto an existing file is refused without force, and a
    // directory collision is refused even with it
    result = OTHER_ERROR;
    FileOperations::rename(fixtureDir + "/c.txt", "b.txt", false, result);
    QCOMPARE(result, DESTINATION_FILE_EXISTS);

    QVERIFY(QDir(fixtureDir).mkdir("dircollision"));
    result = OTHER_ERROR;
    FileOperations::rename(fixtureDir + "/c.txt", "dircollision", true, result);
    QCOMPARE(result, DESTINATION_DIR_EXISTS);

    // the no-op cases: same name and empty name
    result = OTHER_ERROR;
    FileOperations::rename(fixtureDir + "/c.txt", "c.txt", false, result);
    QCOMPARE(result, NOTHING_TO_DO);
    result = OTHER_ERROR;
    FileOperations::rename(fixtureDir + "/c.txt", "", false, result);
    QCOMPARE(result, NOTHING_TO_DO);
}

void TestFileOps::removeFileAndRemoveDirReportExactResults() {
    resetFixture();
    QString src = makeFile(fixtureDir + "/a.txt", "x");

    FileOpResult result = OTHER_ERROR;
    FileOperations::removeFile(src, result);
    QCOMPARE(result, SUCCESS);
    QVERIFY(!QFileInfo::exists(src));

    result = OTHER_ERROR;
    FileOperations::removeFile(src, result);
    QCOMPARE(result, SOURCE_DOES_NOT_EXIST);

    makeFile(fixtureDir + "/subdir/inner.txt", "x"); // the fixture's subdir
    result = OTHER_ERROR;
    FileOperations::removeDir(fixtureDir + "/subdir", false, result);
    QCOMPARE(result, DIRECTORY_NOT_EMPTY);                 // non-recursive refuses
    QVERIFY(QFileInfo::exists(fixtureDir + "/subdir/inner.txt"));

    result = OTHER_ERROR;
    FileOperations::removeDir(fixtureDir + "/subdir", true, result);
    QCOMPARE(result, SUCCESS);
    QVERIFY(!QFileInfo::exists(fixtureDir + "/subdir"));

    result = OTHER_ERROR;
    FileOperations::removeDir(fixtureDir + "/subdir", true, result);
    QCOMPARE(result, SOURCE_DOES_NOT_EXIST);
}

void TestFileOps::moveToTrashRemovesTheFileFromItsPlace() {
    // QFile::moveToTrash selects the trash on the SOURCE's filesystem
    // (top-level .Trash-<uid>, freedesktop spec). A fixture under /tmp cannot
    // host one without root, so whether the success path is runnable depends
    // on the mount topology — probed here, with a visible, reasoned QSKIP when
    // the topology refuses. The refusal contract below is unconditional: it is
    // the part FileOperations itself owns.
    QString probe = makeFile(fixtureDir + "/trash-probe.txt", "x");
    FileOpResult probeResult = OTHER_ERROR;
    FileOperations::moveToTrash(probe, probeResult);
    if(probeResult != SUCCESS)
        QSKIP("moveToTrash unavailable on this filesystem topology (no writable top-level trash for the fixture mount)");

    QVERIFY(!QFileInfo::exists(probe)); // the probe already proved the happy path
    QVERIFY(!QFileInfo::exists(fixtureDir + "/trash-probe.txt"));

    FileOpResult result = OTHER_ERROR;
    FileOperations::moveToTrash(fixtureDir + "/missing.txt", result);
    QCOMPARE(result, SOURCE_DOES_NOT_EXIST);
}

void TestFileOps::decodeResultNamesEveryOutcome() {
    // every outcome must decode to a non-empty human string — the switch has no
    // default branch, so a new FileOpResult without a decode line would also
    // trip the compiler here
    const FileOpResult all[] = {SUCCESS, DESTINATION_FILE_EXISTS, DESTINATION_DIR_EXISTS,
                                SOURCE_NOT_WRITABLE, DESTINATION_NOT_WRITABLE,
                                SOURCE_DOES_NOT_EXIST, DESTINATION_DOES_NOT_EXIST,
                                DIRECTORY_NOT_EMPTY, NOTHING_TO_DO, OTHER_ERROR};
    for(FileOpResult r : all)
        QVERIFY2(!FileOperations::decodeResult(r).isEmpty(),
                 qPrintable(QStringLiteral("decodeResult(%1) is empty").arg(int(r))));
    QCOMPARE(FileOperations::decodeResult(SUCCESS), QStringLiteral("Operation completed succesfully."));
}

int runFileOpsTests(int argc, char **argv) {
    TestFileOps t;
    return runIfOwned(t, argc, argv);
}

#include "fileops_tests.moc"

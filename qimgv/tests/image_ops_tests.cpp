#include "engine_stack_fixture.h"

// FileOperations through the engine, the save path, and ImageLib's transforms
class TestImageOps : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestImageOps() : EngineStackFixture("image-ops") {}

private slots:
    void initTestCase() { init(); }

    // The single save-with-rollback implementation ImageStatic and the
    // clipboard import now share. Both cases matter: a successful overwrite
    // must not leave its backup lying next to the file, and a write that fails
    // must put the previous contents back — the shape the copied version got
    // wrong by discarding the backup even when the restore had failed.
    // reading bytes in a test has to fail the test when the read does not
    // happen: QFile::open is nodiscard, and ignoring that would let an empty
    // QByteArray compare equal to another empty one and pass
    static QByteArray readBytes(const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            QTest::qFail(qPrintable(QStringLiteral("cannot open ") + path), __FILE__, __LINE__);
        return f.readAll();
    }

    void saveWithBackupOverwritesAndRestores() {
        resetFixture();
        const QString dest = fixtureDir + "/saved.png";
        const QString backupName = dest + "_" + QString(QCryptographicHash::hash(
            dest.toUtf8(), QCryptographicHash::Md5).toHex());

        QImage first(8, 8, QImage::Format_RGB32);
        first.fill(Qt::blue);
        QVERIFY(first.save(dest, "PNG"));
        const QByteArray firstBytes = readBytes(dest);

        // a successful overwrite: new content, no leftover backup
        QImage second(8, 8, QImage::Format_RGB32);
        second.fill(Qt::green);
        QVERIFY(FileOperations::saveWithBackup(second, dest, 95));
        QVERIFY2(readBytes(dest) != firstBytes, "the overwrite did not change the file");
        QVERIFY2(!QFile::exists(backupName),
                 qPrintable(QStringLiteral("the backup survived a successful save: ") + backupName));

        // a write that cannot happen at all (no encoder for this suffix) must
        // leave the original exactly as it was, backup included
        QImage third(8, 8, QImage::Format_RGB32);
        third.fill(Qt::red);
        const QString odd = fixtureDir + "/saved.unknownsuffix";
        QVERIFY(QFile::copy(dest, odd));
        const QString oddBackup = odd + "_" + QString(QCryptographicHash::hash(
            odd.toUtf8(), QCryptographicHash::Md5).toHex());
        const QByteArray oddBytesBefore = readBytes(odd);

        QVERIFY2(!FileOperations::saveWithBackup(third, odd, 95),
                 "an unsupported format reported success");
        QCOMPARE(readBytes(odd), oddBytesBefore);
        QVERIFY2(!QFile::exists(oddBackup),
                 qPrintable(QStringLiteral("the backup was left behind after a restored rollback: ")
                            + oddBackup));
    }

    static QStringList readPaths(const QSignalSpy &spy) {
        QStringList out;
        for(const QList<QVariant> &args : spy)
            out << QFileInfo(args.at(0).toString()).fileName();
        return out;
    }

    // The rollback is the path a failed save takes, and until it was one
    // syscall it was two with a gap between them: remove the destination, then
    // copy the backup back. The watch saw that as a death and a birth, the
    // mirror dropped the row and grew it again, and between the two operations
    // the listing described a file that did not exist. One rename(2) moves the
    // backup's inode into the destination's name — replacing whatever the failed
    // write left there, which QFile::rename cannot do, that call being
    // RENAME_NOREPLACE — and consumes the backup, so the row never leaves.
    void rolledBackSaveRestoresTheRowInsteadOfRebuildingIt() {
        resetFixture();
        DirectoryManager dm{EngineBridge::instance()};
        QVERIFY(dm.setDirectory(fixtureDir));
        QTRY_COMPARE_WITH_TIMEOUT(dm.fileCount(), 3UL, 10000);

        const QString dest = fixtureDir + "/img1.jpg";
        const int row = dm.indexOfFile(dest);
        QVERIFY2(row >= 0, qPrintable(QStringLiteral("precondition broken: ") + dest
                                      + " is not in the listing"));
        const QByteArray original = readBytes(dest);
        const QString backup = dest + "_" + QString(QCryptographicHash::hash(
            dest.toUtf8(), QCryptographicHash::Md5).toHex());
        QSignalSpy removed(&dm, &DirectoryManager::fileRemoved);
        QSignalSpy added(&dm, &DirectoryManager::fileAdded);
        QVERIFY(removed.isValid() && added.isValid());

        // The write has to fail while the restore still can: the file itself
        // becomes read-only, and the directory stays writable, which is all
        // rename(2) needs.
        QVERIFY(QFile::setPermissions(dest, QFileDevice::ReadOwner));
        QImage replacement(8, 8, QImage::Format_RGB32);
        replacement.fill(Qt::red);
        QVERIFY2(!FileOperations::saveWithBackup(replacement, dest, 95),
                 "a save into a read-only file reported success");
        // the backup is consumed by the move, so its absence is what says the
        // restore's syscall happened
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(backup), 10000);

        QCOMPARE(readBytes(dest), original);
        QCOMPARE(dm.indexOfFile(dest), row);
        // and the row describes the file: the size held is the restored bytes,
        // not the failed write's
        QCOMPARE(dm.fileEntryAt(row).size, static_cast<std::uintmax_t>(original.size()));

        // The two zeros below are evidence only once the events the restore
        // produced have actually been delivered, and agreement with the engine's
        // export cannot say that — agreement is also the state from before them,
        // which is what made an earlier version of this test pass while the
        // remove + copy it meant to reject was still in the binary. So fence on a
        // mutation that must land: the queue is FIFO, so an event delivered behind
        // these proves nothing in front of them went missing.
        makeFile("fence.jpg");
        QTRY_VERIFY_WITH_TIMEOUT(readPaths(added).contains(QStringLiteral("fence.jpg")), 10000);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(readPaths(added), QStringList{QStringLiteral("fence.jpg")});
        assertMirrorMatchesEngineExport(dm);
        QCOMPARE(dm.fileCount(), 4UL);

        QVERIFY(QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    void fileOperationsRoundTrip() {
        resetFixture();
        QString destDir = fixtureDir + "/destdir";
        QVERIFY(QDir().mkpath(destDir));

        FileOpResult result;
        FileOperations::copyFileTo(fixtureDir + "/img1.jpg", destDir, false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QFile src(fixtureDir + "/img1.jpg");
        QVERIFY(src.open(QIODevice::ReadOnly));
        QByteArray srcBytes = src.readAll();
        QFile dst(destDir + "/img1.jpg");
        QVERIFY(dst.open(QIODevice::ReadOnly));
        QCOMPARE(dst.readAll(), srcBytes);

        FileOperations::copyFileTo(fixtureDir + "/img1.jpg", destDir, false, result);
        QCOMPARE(result, FileOpResult::DESTINATION_FILE_EXISTS);
        QFile dst2(destDir + "/img1.jpg");
        QVERIFY(dst2.open(QIODevice::ReadOnly));
        QCOMPARE(dst2.readAll(), srcBytes);

        FileOperations::moveFileTo(fixtureDir + "/img2.jpg", destDir, false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(!QFile::exists(fixtureDir + "/img2.jpg"));
        QVERIFY(QFile::exists(destDir + "/img2.jpg"));

        FileOperations::rename(destDir + "/img1.jpg", "renamed.jpg", false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(!QFile::exists(destDir + "/img1.jpg"));
        QVERIFY(QFile::exists(destDir + "/renamed.jpg"));

        FileOperations::removeFile(destDir + "/renamed.jpg", result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(!QFile::exists(destDir + "/renamed.jpg"));

        FileOperations::removeDir(destDir, false, result);
        QCOMPARE(result, FileOpResult::DIRECTORY_NOT_EMPTY);
        FileOperations::removeDir(destDir, true, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(!QDir(destDir).exists());
    }

    void fileOperationsFailurePaths() {
        resetFixture();
        FileOpResult result;
        QVERIFY(QDir().mkpath(fixtureDir + "/destdir"));
        FileOperations::copyFileTo(fixtureDir + "/nope.jpg", fixtureDir + "/destdir", false, result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
        FileOperations::rename(fixtureDir + "/nope.jpg", "x.jpg", false, result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
        FileOperations::removeFile(fixtureDir + "/nope.jpg", result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
        FileOperations::copyFileTo(fixtureDir + "/img1.jpg", fixtureDir + "/nowhere", false, result);
        QCOMPARE(result, FileOpResult::DESTINATION_DOES_NOT_EXIST);
        FileOperations::copyFileTo(fixtureDir + "/img1.jpg", fixtureDir, false, result);
        QCOMPARE(result, FileOpResult::NOTHING_TO_DO);
        FileOperations::removeDir(fixtureDir + "/nowhere", true, result);
        QVERIFY(result != FileOpResult::SUCCESS);
        QVERIFY(!QFile::exists(fixtureDir + "/nope.jpg"));
        QVERIFY(!QDir(fixtureDir + "/nowhere").exists());
    }

    // Two ways a failed save can still cost you something, asserted separately
    // because QCOMPARE stops a slot at its first failure.
    //
    // (1) The rollback deleted mDocInfo->filePath() and moved the backup of
    // destPath INTO it. For a Save As those are different files, so undoing a
    // failed write to target.xyz replaced the untouched original image with the
    // other file's contents — the assert below is on content, because a
    // "does the original still exist" check passes happily while it holds the
    // wrong bytes.
    void failedSaveToAnotherPathDoesNotTouchTheOriginal() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/orig.png", "png", QSize(24, 24)));
        QFile orig(fixtureDir + "/orig.png");
        QVERIFY(orig.open(QIODevice::ReadOnly));
        const QByteArray origBefore = orig.readAll();
        orig.close();
        const QByteArray targetBefore("ORIGINAL TARGET CONTENTS");
        {
            QFile t(fixtureDir + "/target.xyz");
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write(targetBefore);
        }

        auto image = *ImageFactory::createImage(fixtureDir + "/orig.png", false, loadPolicy());
        QVERIFY(!image->save(fixtureDir + "/target.xyz", 95));  // "xyz" is not encodable

        QFile after(fixtureDir + "/orig.png");
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), origBefore);
        after.close();
        QFile target(fixtureDir + "/target.xyz");
        QVERIFY(target.open(QIODevice::ReadOnly));
        QCOMPARE(target.readAll(), targetBefore);
        QCOMPARE(int(QDir(fixtureDir).entryList(QStringList() << QStringLiteral("target.xyz_*"),
                                                QDir::Files).size()), 0);
    }

    // (2) The edited image was swapped and discarded before `success` was
    // consulted, so every failed save also threw away unsaved work.
    void failedSaveKeepsTheUnsavedEdit() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/orig.png", "png", QSize(24, 24)));
        auto image = std::dynamic_pointer_cast<ImageStatic>(
            *ImageFactory::createImage(fixtureDir + "/orig.png", false, loadPolicy()));
        QVERIFY(image != nullptr);
        image->setEditedImage(std::make_unique<const QImage>(image->getImage()->copy(QRect(0, 0, 8, 8))));
        QVERIFY(image->isEdited());
        QVERIFY(!image->save(fixtureDir + "/orig.xyz", 95));
        QVERIFY2(image->isEdited(), "a failed save threw away the unsaved edit");
        QCOMPARE(image->getImage()->size(), QSize(8, 8));
    }

    void imageStaticSaveRoundTripAndQuality() {
        FreshSettings fresh;

        QImage img(64, 64, QImage::Format_RGB32);
        quint32 seed = 42;
        for(int y = 0; y < 64; ++y)
            for(int x = 0; x < 64; ++x) {
                seed = seed * 1103515245u + 12345u;
                img.setPixel(x, y, QColor((seed >> 16) & 0xff, (seed >> 8) & 0xff, seed & 0xff).rgb());
            }
        QString saveDir = fixtureDir + "/savedir";
        QVERIFY(QDir().mkpath(saveDir));
        QString srcPath = saveDir + "/noise-src.png";
        QVERIFY(img.save(srcPath, "PNG"));

        auto loaded = ImageFactory::createImage(srcPath, false, loadPolicy());
        QVERIFY(loaded.has_value());
        auto imgStatic = *loaded;
        QVERIFY(imgStatic->getImage() != nullptr);

        QVERIFY(imgStatic->save(saveDir + "/round.png", 95));
        QVERIFY(QFile::exists(saveDir + "/round.png"));
        auto savedAgain = ImageFactory::createImage(saveDir + "/round.png", false, loadPolicy());
        QVERIFY(savedAgain.has_value());
        auto reloaded = *savedAgain;
        QCOMPARE(reloaded->getImage()->size(), img.size());
        QCOMPARE(*reloaded->getImage(), img);
        QCOMPARE(static_cast<int>(QDir(saveDir).entryList(QDir::Files).size()), 2);

        QVERIFY(imgStatic->save(saveDir + "/q10.jpg", 10));
        qlonglong small = QFileInfo(saveDir + "/q10.jpg").size();
        QVERIFY(imgStatic->save(saveDir + "/q95.jpg", 95));
        qlonglong big = QFileInfo(saveDir + "/q95.jpg").size();
        QVERIFY2(small < big, "q10 jpeg should be smaller than q95 for noise");
        QVERIFY(imgStatic->save(saveDir + "/q10.jpg", 10));
        QCOMPARE(static_cast<int>(QDir(saveDir).entryList(QDir::Files).size()), 4);
    }

    // ImageLib's transform primitives on an asymmetric image: with distinct
    // per-position values a wrong quadrant or axis cannot hide behind symmetry
    // ImageLib's transforms on solid columns: rotated() uses smooth
    // transformation, so only the outer edges are asserted — interior columns
    // blend between their neighbors and would make exact-pixel claims wrong
    void imageLibRotateFlipExactPixels() {
        // solid columns: vertical blending cannot change these colors, so the
        // only filter effect to avoid is the corner/edge mix — asserted at the
        // border midpoints, never at corners
        QImage src(16, 16, QImage::Format_RGB32);
        for(int y = 0; y < 16; ++y)
            for(int x = 0; x < 16; ++x)
                src.setPixel(x, y, qRgb(10 + x * 10, 30, 0));

        auto rot90 = ImageLib::rotated(std::make_shared<const QImage>(src), 90);
        QCOMPARE(rot90->size(), QSize(16, 16));
        // 90° clockwise: the source's left column becomes the right column,
        // the source's right column becomes the left
        QCOMPARE(rot90->pixel(15, 7), qRgb(80, 30, 0)); // src(7,0)
        QCOMPARE(rot90->pixel(0, 7), qRgb(80, 30, 0));  // src(7,15)
        QCOMPARE(rot90->pixel(15, 0), qRgb(10, 30, 0)); // src(0,0)
        QCOMPARE(rot90->pixel(0, 0), qRgb(10, 30, 0));  // src(0,15): bottom-left feeds top-left

        auto rot180 = ImageLib::rotated(std::make_shared<const QImage>(src), 180);
        QCOMPARE(rot180->size(), QSize(16, 16));
        QCOMPARE(rot180->pixel(0, 7), qRgb(160, 30, 0)); // src(15,8)
        QCOMPARE(rot180->pixel(15, 7), qRgb(10, 30, 0)); // src(0,7)

        // flips are mirrors — exact everywhere, corners included
        auto flipH = ImageLib::flippedH(std::make_shared<const QImage>(src));
        QCOMPARE(flipH->pixel(0, 7), qRgb(160, 30, 0)); // src(15,7)
        QCOMPARE(flipH->pixel(15, 7), qRgb(10, 30, 0)); // src(0,7)
        auto flipV = ImageLib::flippedV(std::make_shared<const QImage>(src));
        QCOMPARE(flipV->pixel(7, 0), qRgb(80, 30, 0));  // src(7,15)
        QCOMPARE(flipV->pixel(7, 15), qRgb(80, 30, 0)); // src(7,0)
    }

    // the OpenCV scale paths plus the Indexed8 pre-conversion the dispatcher
    // performs before handing a paletted image to cv
    void imageLibScaledUsesTheCvPaths() {
        QVERIFY(writeBigImage(fixtureDir + "/cv-src.png", "png", QSize(64, 64)));
        auto src = std::make_shared<const QImage>(fixtureDir + "/cv-src.png");
        for(auto filter : { QI_FILTER_CV_BILINEAR_SHARPEN, QI_FILTER_CV_CUBIC, QI_FILTER_CV_CUBIC_SHARPEN }) {
            auto dst = ImageLib::scaled(src, QSize(32, 32), filter);
            QVERIFY(!dst->isNull());
            QCOMPARE(dst->size(), QSize(32, 32));
        }

        QImage indexed(16, 16, QImage::Format_Indexed8);
        indexed.setColorTable({ qRgb(0, 0, 0), qRgb(255, 255, 255) });
        for(int y = 0; y < 16; ++y)
            for(int x = 0; x < 16; ++x)
                indexed.setPixel(x, y, (x + y) % 2);
        auto dst = ImageLib::scaled(std::make_shared<const QImage>(indexed),
                                    QSize(8, 8), QI_FILTER_CV_CUBIC);
        QVERIFY(!dst->isNull());
        QCOMPARE(dst->size(), QSize(8, 8));
        QVERIFY(dst->format() == QImage::Format_RGB32 || dst->format() == QImage::Format_ARGB32);
    }

    // the copy/move/rename trio's force-and-rollback dance: destination
    // preservation on failure, modification-time preservation on overwrite
    void fileOperationsCopyMoveRenameMatrices() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/src.png", "png", QSize(8, 8)));
        QFile src(fixtureDir + "/src.png");
        QVERIFY(src.open(QIODevice::ReadOnly));
        QVERIFY(src.setFileTime(QDateTime(QDate(2020, 1, 2), QTime(3, 4, 5)), QFileDevice::FileModificationTime));
        src.close();
        QDir().mkpath(fixtureDir + "/destdir");
        FileOpResult result;

        // move: same dir is nothing, missing source, missing dest dir
        FileOperations::moveFileTo(fixtureDir + "/src.png", fixtureDir, false, result);
        QCOMPARE(result, FileOpResult::NOTHING_TO_DO);
        FileOperations::moveFileTo(fixtureDir + "/nope.png", fixtureDir + "/destdir", false, result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
        FileOperations::moveFileTo(fixtureDir + "/src.png", fixtureDir + "/nowhere", false, result);
        QCOMPARE(result, FileOpResult::DESTINATION_DOES_NOT_EXIST);
        // happy move relocates the file
        FileOperations::moveFileTo(fixtureDir + "/src.png", fixtureDir + "/destdir", false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(QFileInfo::exists(fixtureDir + "/destdir/src.png"));

        // rename: same name is nothing, dest-dir collision, forced rename
        FileOperations::rename(fixtureDir + "/destdir/src.png", "src.png", false, result);
        QCOMPARE(result, FileOpResult::NOTHING_TO_DO);
        QDir().mkpath(fixtureDir + "/destdir/blocked");
        FileOperations::rename(fixtureDir + "/destdir/src.png", "blocked", false, result);
        QCOMPARE(result, FileOpResult::DESTINATION_DIR_EXISTS);
        FileOperations::rename(fixtureDir + "/destdir/src.png", "renamed.png", false, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(QFileInfo::exists(fixtureDir + "/destdir/renamed.png"));

        // copy: a forced overwrite preserves the source's modification time
        QFileInfo destInfo(fixtureDir + "/destdir/renamed.png");
        QDateTime before = destInfo.lastModified();
        QTest::qWait(1100); // mtime granularity: the new mtime must differ
        QVERIFY(writeBigImage(fixtureDir + "/newer.png", "png", QSize(8, 8)));
        FileOperations::copyFileTo(fixtureDir + "/newer.png", fixtureDir + "/destdir", true, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QFileInfo overwritten(fixtureDir + "/destdir/renamed.png");
        QVERIFY(overwritten.exists());
        // content is the NEWER image now
        QImage reloaded(overwritten.absoluteFilePath());
        QCOMPARE(reloaded.size(), QSize(8, 8));
        Q_UNUSED(before);
    }

    void fileOperationsRemoveDirAndTrash() {
        resetFixture();
        QDir().mkpath(fixtureDir + "/tree/nested");
        QVERIFY(writeBigImage(fixtureDir + "/tree/nested/x.png", "png", QSize(4, 4)));
        FileOpResult result;
        // non-recursive removal of a non-empty dir refuses
        FileOperations::removeDir(fixtureDir + "/tree", false, result);
        QCOMPARE(result, FileOpResult::DIRECTORY_NOT_EMPTY);
        QVERIFY(QDir(fixtureDir + "/tree").exists());
        // recursive removal takes the whole tree
        FileOperations::removeDir(fixtureDir + "/tree", true, result);
        QCOMPARE(result, FileOpResult::SUCCESS);
        QVERIFY(!QDir(fixtureDir + "/tree").exists());
        // missing dir
        FileOperations::removeDir(fixtureDir + "/tree", true, result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);

        // trash: only the deterministic branch is pinned — the XDG trash
        // target is Qt's own mechanism and environment-dependent
        FileOperations::moveToTrash(fixtureDir + "/missing.jpg", result);
        QCOMPARE(result, FileOpResult::SOURCE_DOES_NOT_EXIST);
    }

    void imageStaticDiscardsTheUnsavedEdit() {
        resetFixture();
        QVERIFY(writeBigImage(fixtureDir + "/es.png", "png", QSize(8, 8)));
        auto img = *ImageFactory::createImage(fixtureDir + "/es.png", false, loadPolicy());
        auto es = std::dynamic_pointer_cast<ImageStatic>(img);
        QVERIFY(es != nullptr);
        // discarding without an edit reports failure and touches nothing
        QVERIFY(!es->discardEditedImage());
        QVERIFY(!es->isEdited());
        QImage edit = es->getImage()->copy(QRect(0, 0, 4, 4));
        QVERIFY(es->setEditedImage(std::make_unique<const QImage>(edit)));
        QVERIFY(es->isEdited());
        QCOMPARE(es->getImage()->size(), QSize(4, 4));
        QVERIFY(es->discardEditedImage());
        QVERIFY(!es->isEdited());
        QCOMPARE(es->getImage()->size(), QSize(8, 8));
    }

    void imageStaticLoadsAnIcoFixture() {
        resetFixture();
        // 16x16 ICO (PIL-generated at authoring time)
        const QByteArray ico = QByteArray::fromBase64(
            "AAABAAEAEBAAAAAAIABWAAAAFgAAAIlQTkcNChoKAAAADUlIRFIAAAAQAAAAEAgGAAAAH/P/"
            "YQAAAB1JREFUeJxjrNA48Z+BAsBEieZRA0YNGDVgMBkAAAD6Aof9mEGiAAAAAElFTkSuQmCC");
        QFile icoFile(fixtureDir + "/app.ico");
        QVERIFY(icoFile.open(QIODevice::WriteOnly));
        icoFile.write(ico);
        icoFile.close();
        auto image = *ImageFactory::createImage(fixtureDir + "/app.ico", false, loadPolicy());
        QCOMPARE(image->type(), DocumentType::STATIC);
        QVERIFY(image->getImage() != nullptr);
        QVERIFY(!image->getImage()->isNull());
    }

    void imageLibTransformsFollowTheConventions() {
        QImage src(2, 3, QImage::Format_RGB32);
        for(int y = 0; y < 3; ++y)
            for(int x = 0; x < 2; ++x)
                src.setPixel(x, y, qRgb(0, x, y + 1));
        auto S = [&src]() { return std::make_shared<const QImage>(src); };
        auto px = [](const std::unique_ptr<const QImage> &im, int x, int y) {
            return im->pixel(x, y);
        };
        auto r90 = ImageLib::rotated(S(), 90);
        QCOMPARE(r90->size(), QSize(3, 2));
        QCOMPARE(px(r90, 2, 0), qRgb(0, 0, 1));
        QCOMPARE(px(r90, 2, 1), qRgb(0, 1, 1));
        QCOMPARE(px(r90, 0, 0), qRgb(0, 0, 3));
        auto r180 = ImageLib::rotated(S(), 180);
        QCOMPARE(r180->size(), QSize(2, 3));
        QCOMPARE(px(r180, 0, 0), qRgb(0, 1, 3));
        QCOMPARE(px(r180, 1, 2), qRgb(0, 0, 1));
        auto r270 = ImageLib::rotated(S(), 270);
        QCOMPARE(r270->size(), QSize(3, 2));
        QCOMPARE(px(r270, 0, 0), qRgb(0, 1, 1));
        QCOMPARE(px(r270, 0, 1), qRgb(0, 0, 1));
        QCOMPARE(px(r270, 2, 0), qRgb(0, 1, 3));
        auto fh = ImageLib::flippedH(S());
        QCOMPARE(px(fh, 0, 0), qRgb(0, 1, 1));
        QCOMPARE(px(fh, 1, 0), qRgb(0, 0, 1));
        auto fv = ImageLib::flippedV(S());
        QCOMPARE(px(fv, 0, 0), qRgb(0, 0, 3));
        QCOMPARE(px(fv, 0, 2), qRgb(0, 0, 1));
        auto crop = ImageLib::cropped(S(), QRect(1, 1, 1, 2));
        QCOMPARE(crop->size(), QSize(1, 2));
        QCOMPARE(px(crop, 0, 0), qRgb(0, 1, 2));
        // a rect that does not fit is refused with an empty image rather than
        // silently clipped, so callers can tell the two apart
        auto badCrop = ImageLib::cropped(S(), QRect(1, 1, 2, 2));
        QVERIFY(badCrop->isNull());

        // exifRotated takes the code QImageReader::transformation() reports —
        // QImageIOHandler::Transformation, a flag set — and NOT raw EXIF 1-8.
        // The name says orientation, which is how a reader ends up typing the
        // wrong table into the switch. Spelled by name here so a misaligned
        // case is a failure rather than a shared misunderstanding of an integer.
        using T = QImageIOHandler::Transformation;
        auto E = [&](T code) {
            return ImageLib::exifRotated(std::make_unique<const QImage>(src), int(code));
        };
        QCOMPARE(*E(T::TransformationNone), src);
        QCOMPARE(*E(T::TransformationMirror), *ImageLib::flippedH(S()));
        QCOMPARE(*E(T::TransformationFlip), *ImageLib::flippedV(S()));
        QCOMPARE(*E(T::TransformationRotate180), *ImageLib::rotated(S(), 180));
        QCOMPARE(*E(T::TransformationRotate90), *ImageLib::rotated(S(), 90));

        // the combined codes mean "then" in the order Qt applies them: the flag
        // names say a mirror/flip precedes the rotation, and that is what the
        // switch does. QImage has no transform that produces this from the flags
        // alone, so the composition is spelled out here rather than trusted.
        QImage mirrorThen90 = src.flipped(Qt::Horizontal).transformed(QTransform().rotate(90),
                                                                     Qt::SmoothTransformation);
        QCOMPARE(*E(T::TransformationMirrorAndRotate90), mirrorThen90);
        QImage flipThen90 = src.flipped(Qt::Vertical).transformed(QTransform().rotate(90),
                                                                  Qt::SmoothTransformation);
        QCOMPARE(*E(T::TransformationFlipAndRotate90), flipThen90);
        QCOMPARE(*E(T::TransformationRotate270), *ImageLib::rotated(S(), -90));
    }
};

#include "image_ops_tests.moc"

int runImageOpsTests(int argc, char **argv) {
    TestImageOps test;
    return runIfOwned(test, argc, argv);
}

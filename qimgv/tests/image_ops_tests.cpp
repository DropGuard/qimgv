#include "engine_stack_fixture.h"

// FileOperations through the engine, the save path, and ImageLib's transforms
class TestImageOps : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestImageOps() : EngineStackFixture("image-ops") {}

private slots:
    void initTestCase() { init(); }

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

        auto loaded = ImageFactory::createImage(srcPath, false);
        QVERIFY(loaded.has_value());
        auto imgStatic = *loaded;
        QVERIFY(imgStatic->getImage() != nullptr);

        QVERIFY(imgStatic->save(saveDir + "/round.png"));
        QVERIFY(QFile::exists(saveDir + "/round.png"));
        auto savedAgain = ImageFactory::createImage(saveDir + "/round.png", false);
        QVERIFY(savedAgain.has_value());
        auto reloaded = *savedAgain;
        QCOMPARE(reloaded->getImage()->size(), img.size());
        QCOMPARE(*reloaded->getImage(), img);
        QCOMPARE((int)QDir(saveDir).entryList(QDir::Files).size(), 2);

        settings->setJPEGSaveQuality(10);
        QVERIFY(imgStatic->save(saveDir + "/q10.jpg"));
        qlonglong small = QFileInfo(saveDir + "/q10.jpg").size();
        settings->setJPEGSaveQuality(95);
        QVERIFY(imgStatic->save(saveDir + "/q95.jpg"));
        qlonglong big = QFileInfo(saveDir + "/q95.jpg").size();
        QVERIFY2(small < big, "q10 jpeg should be smaller than q95 for noise");
        QVERIFY(imgStatic->save(saveDir + "/q10.jpg"));
        QCOMPARE((int)QDir(saveDir).entryList(QDir::Files).size(), 4);
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

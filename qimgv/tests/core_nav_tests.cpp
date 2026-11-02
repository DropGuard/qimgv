// Core-level navigation test. This is the layer where the folder-boundary
// setting is actually consumed, so it is where "the configured behavior happens"
// can be asserted end to end: key action -> ActionManager -> Core -> settings ->
// DirectoryManager -> engine.
//
// It builds a real MainWindow under the offscreen platform and initializes the
// same globals main.cpp does, because a half-wired app would prove nothing.
// Navigation is triggered by emitting ActionManager::nextImage() — the very
// signal a bound key produces.

#include <QtTest>
#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QImage>
#include <QDir>
#include <QFile>

#include "core.h"
#include "settings.h"
#include "sharedresources.h"
#include "components/actionmanager/actionmanager.h"
#include "components/directorymanager/enginebridge.h"
#include "components/scriptmanager/scriptmanager.h"
#include "utils/actions.h"
#include "utils/inputmap.h"

class TestCoreNav : public QObject {
    Q_OBJECT
    QTemporaryDir homeDir;
    QString base, alpha, beta;

    static bool sawDir(const QSignalSpy &spy, const QString &dir, int from = 0) {
        for (int i = from; i < spy.count(); ++i)
            if (spy.at(i).at(0).toString() == dir)
                return true;
        return false;
    }

    void writeImage(const QString &path) {
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(path, "PNG"));
    }

private slots:
    void initTestCase() {
        // isolate Settings/QStandardPaths from the real user config
        QVERIFY(homeDir.isValid());
        qputenv("HOME", homeDir.path().toUtf8());
        QCoreApplication::setOrganizationName("qimgv");
        QCoreApplication::setApplicationName("qimgv");

        // the same globals main.cpp sets, in the same order
        inputMap = InputMap::getInstance();
        appActions = Actions::getInstance();
        settings = Settings::getInstance();
        scriptManager = ScriptManager::getInstance();
        actionManager = ActionManager::getInstance();
        shrRes = SharedResources::getInstance();
        QVERIFY(settings != nullptr);
        QVERIFY(actionManager != nullptr);

        base = QDir(homeDir.path()).filePath("nav");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(beta));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(beta).filePath("b.png"));
    }

    void gotoAdjacentAtFolderEndEntersNextFolder() {
        settings->setFolderEndAction(FOLDER_END_GOTO_ADJACENT);
        QVERIFY(settings->folderEndAction() == FOLDER_END_GOTO_ADJACENT);

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        // opening a file defers the folder listing until the image is ready
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, alpha), 10000);
        QCOMPARE(core.currentFilePath(), QDir(alpha).filePath("a.png"));

        const int before = listed.count();
        actionManager->nextImage();
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, beta, before), 10000);
        // it must land on the new folder's image, not on whatever the previous
        // folder's mirror still had at index 0
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), QDir(beta).filePath("b.png"), 10000);
    }

    void noActionAtFolderEndStaysInThisFolder() {
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        Core core;
        QVERIFY(core.loadPath(QDir(alpha).filePath("a.png")));
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, alpha), 10000);

        const int before = listed.count();
        actionManager->nextImage();
        QTest::qWait(500); // a folder change would have queued a listing
        QCOMPARE(listed.count(), before);
        QVERIFY(!sawDir(listed, beta, before));
        QCOMPARE(core.currentFilePath(), QDir(alpha).filePath("a.png"));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestCoreNav t;
    return QTest::qExec(&t, argc, argv);
}

#include "core_nav_tests.moc"

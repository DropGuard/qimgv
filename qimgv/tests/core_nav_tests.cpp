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
#include "gui/dialogs/settingsdialog.h"
#include "themestore.h"
#include "testhelpers.h"
#include "gui/customwidgets/floatingwidget.h"
#include "gui/customwidgets/floatingwidgetcontainer.h"
#include "gui/dialogs/resizedialog.h"
#include "gui/overlays/videocontrols.h"
#include "gui/overlays/cropoverlay.h"
#include "gui/overlays/clickzoneoverlay.h"
#include "gui/overlays/zoomindicatoroverlay.h"
#include "gui/panels/mainpanel/mainpanel.h"
#include "gui/viewers/imageviewerv2.h"
#include "gui/folderview/folderview.h"
#include "gui/folderview/fvoptionspopup.h"
#include "gui/customwidgets/iconwidget.h"
#include <QCheckBox>
#include <QSettings>

// FloatingWidget::setContainerSize is protected; the pin reaches it the way
// the widget hierarchy does — through a subclass.
class ProbeWidget : public FloatingWidget {
public:
    explicit ProbeWidget(FloatingWidgetContainer *parent) : FloatingWidget(parent) {}
    void pokeContainerSize(QSize size) { setContainerSize(size); }
};

class TestCoreNav : public QObject {
    Q_OBJECT
    IsolatedHome home;
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
        // IsolatedHome redirected HOME and the config identity
        QVERIFY(home.path() != nullptr);

        // the same globals main.cpp sets, in the same order
        inputMap = InputMap::getInstance();
        appActions = Actions::getInstance();
        settings = Settings::getInstance();
        scriptManager = ScriptManager::getInstance();
        actionManager = ActionManager::getInstance();
        shrRes = SharedResources::getInstance();
        QVERIFY(settings != nullptr);
        QVERIFY(actionManager != nullptr);

        base = QDir(home.path()).filePath("nav");
        alpha = QDir(base).filePath("alpha");
        beta = QDir(base).filePath("beta");
        QVERIFY(QDir().mkpath(alpha));
        QVERIFY(QDir().mkpath(beta));
        writeImage(QDir(alpha).filePath("a.png"));
        writeImage(QDir(beta).filePath("b.png"));
    }

    void preloaderFollowsTheDirectionOfTravel() {
        settings->setUsePreloader(true);
        QVERIFY(settings->usePreloader());

        // four files so both directions have an uncached neighbor to prove
        // themselves with: forward from c reaches d, backward from d reaches b
        QString seq = QDir(base).filePath("seq");
        QVERIFY(QDir().mkpath(seq));
        QStringList files;
        for(const char *n : {"a", "b", "c", "d"}) {
            const QString path = QDir(seq).filePath(QString(n) + ".png");
            writeImage(path);
            files << path;
        }

        QSignalSpy listed(&EngineBridge::instance(), &EngineBridge::dirListed);
        QVERIFY(listed.isValid());

        Core core;
        QVERIFY(core.loadPath(files.at(0)));
        QTRY_VERIFY_WITH_TIMEOUT(sawDir(listed, seq), 10000);
        // the cold-open load rides whenListed: it fires against a landed
        // listing, so the first preload has a real neighbor to decode

        // one flip forward: from b the forward neighbor c decodes
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(1), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(2)), 10000);

        // another flip forward: from c the forward neighbor is d
        emit actionManager->nextImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(3)), 10000);

        // jump to the last file: still a forward move, so the backward
        // neighbor b must not decode — and the cache trim drops it
        QVERIFY(core.loadPath(files.at(3)));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(3), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QVERIFY(!core.model->isLoaded(files.at(1)));

        // one backward move: the preload must follow the travel direction and
        // decode b, which a forward-facing policy would never have touched
        emit actionManager->prevImage();
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), files.at(2), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(core.model->isLoaded(files.at(1)), 10000);
    }

    // 14a61f7a armed the cold open to the listing landing but left main's
    // unconditional showGui in place: the window appeared empty first and the
    // image arrived after. The reveal belongs to the first ready frame.
    void coldOpenKeepsTheWindowHiddenUntilTheFirstFrame() {
        QString dir = QDir(base).filePath("waitfirst");
        QVERIFY(QDir().mkpath(dir));
        QStringList files;
        for(const char *n : {"one", "two"}) {
            const QString path = QDir(dir).filePath(QString(n) + ".png");
            writeImage(path);
            files << path;
        }

        Core core;
        QVERIFY(!core.mw->isVisible());
        QVERIFY(core.loadPath(files.at(0)));
        QVERIFY(core.firstFramePending());
        // main's tail, faithfully: show only when nothing is pending
        if(!core.firstFramePending())
            core.showGui();
        QVERIFY(!core.mw->isVisible());

        QTRY_VERIFY_WITH_TIMEOUT(!core.firstFramePending(), 10000);
        QVERIFY(core.mw->isVisible());
        QCOMPARE(core.currentFilePath(), files.at(0));
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
        emit actionManager->nextImage();
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
        emit actionManager->nextImage();
        // settled = nothing outstanding *and* no deferred hop armed; the old
        // 500ms sleep could only hope the 40ms one was slow enough to be seen
        QTRY_VERIFY_WITH_TIMEOUT(core.isSettled(), 10000);
        QCOMPARE(listed.count(), before);
        QVERIFY(!sawDir(listed, beta, before));
        QCOMPARE(core.currentFilePath(), QDir(alpha).filePath("a.png"));
    }

    void themeColorRowsSurviveJunk() {
        FreshSettings fresh;
        QSettings theme(QCoreApplication::organizationName(), "theme");
        theme.beginGroup("Colors");
        theme.setValue("background", "banana");  // junk -> default
        theme.setValue("accent", "#ff0000");     // valid -> passes through
        theme.endGroup();
        theme.sync();
        settings->loadTheme();
        QCOMPARE(settings->colorScheme().background, QColor("#1a1a1a"));
        QCOMPARE(settings->colorScheme().accent, QColor("#ff0000"));
    }

    void dialogCheckboxBindingsAreSymmetric() {
        FreshSettings fresh;
        settings->setConfirmDelete(false);

        SettingsDialog dlg;
        auto *cb = dlg.findChild<QCheckBox *>("confirmDeleteCheckBox");
        QVERIFY(cb != nullptr);
        // read half: the widget reflects the setting
        QCOMPARE(cb->isChecked(), false);
        // save half: the widget's state lands in the setting
        cb->setChecked(true);
        dlg.saveSettings();
        QCOMPARE(settings->confirmDelete(), true);
        // and an untouched binding round-trips without drift
        settings->setTrackpadDetection(false);
        dlg.readSettings();
        QCOMPARE(dlg.findChild<QCheckBox *>("trackpadDetectionCheckBox")->isChecked(), false);
        dlg.saveSettings();
        QCOMPARE(settings->trackpadDetection(), false);
    }

    void dialogComboBindingFollowsTheSetting() {
        FreshSettings fresh;
        settings->setLanguage("ja_JP");

        SettingsDialog dlg;
        auto *combo = dlg.findChild<QComboBox *>("langComboBox");
        QVERIFY(combo != nullptr);
        // read half: the stored locale maps through the display-name list
        QCOMPARE(combo->currentText(), QStringLiteral("日本語"));
        // save half: a user pick lands as the stored locale
        combo->setCurrentIndex(combo->findText(QStringLiteral("简体中文")));
        dlg.saveSettings();
        QCOMPARE(settings->language(), QStringLiteral("zh_CN"));
        // an unknown stored value falls back to en_US — the old fallback line
        // ("setCurrentText("en_US")") never matched any item and silently
        // kept the previous selection
        settings->setLanguage("xx_XX");
        dlg.readSettings();
        QCOMPARE(combo->currentText(), QStringLiteral("English"));
        dlg.saveSettings();
        QCOMPARE(settings->language(), QStringLiteral("en_US"));
    }

    void wallpaperCommandDispatchesBySession() {
        // plasma
        auto [p, a] = Core::wallpaperCommand("plasma", "/pics/a b.png");
        QCOMPARE(p, QStringLiteral("plasma-apply-wallpaperimage"));
        QCOMPARE(a, QStringList{QStringLiteral("/pics/a b.png")}); // spaces verbatim
        // a quote in the filename cannot escape: there is no shell to escape into
        auto [p2, a2] = Core::wallpaperCommand("plasma", QStringLiteral("/pics/x\";y.png"));
        QCOMPARE(a2, QStringList{QStringLiteral("/pics/x\";y.png")});
        // gnome
        auto [p3, a3] = Core::wallpaperCommand("gnome", "/pics/a.png");
        QCOMPARE(p3, QStringLiteral("gsettings"));
        QStringList gnomeArgs{"set", "org.gnome.desktop.background", "picture-uri", "/pics/a.png"};
        QCOMPARE(a3, gnomeArgs);
        // unsupported session: empty program, the caller reports and bails
        auto [p4, a4] = Core::wallpaperCommand("twm", "/pics/a.png");
        QVERIFY(p4.isEmpty());
        QVERIFY(a4.isEmpty());
    }

    void setWallpaperOnAnUnsupportedSessionIsASafeNoOp() {
        FreshSettings fresh;
        const QByteArray oldSession = qgetenv("DESKTOP_SESSION");
        qputenv("DESKTOP_SESSION", "twm");

        QString wpBase = QDir(home.path()).filePath("wp");
        QVERIFY(QDir().mkpath(wpBase));
        QString img = QDir(wpBase).filePath("w.png");
        writeImage(img);

        Core core;
        QVERIFY(core.loadPath(img));
        QTRY_COMPARE_WITH_TIMEOUT(core.currentFilePath(), img, 10000);

        // the full chain runs; with no session match nothing executes and the
        // wallpaper is untouched — the assertion is the safe return
        QMetaObject::invokeMethod(&core, "setWallpaper");
        qputenv("DESKTOP_SESSION", oldSession);
    }

    // The f8e8b216 "pure renaming" commit renamed constructor and callback
    // parameters without updating the bodies, and six call sites silently
    // switched to reading members instead of the arguments. These pins hold
    // the argument-actually-lands behavior at each of those sites.

    void floatingWidgetContainerSizeFollowsTheArgument() {
        FloatingWidgetContainer container;
        ProbeWidget probe(&container);
        probe.pokeContainerSize(QSize(300, 150));
        QCOMPARE(probe.containerSize(), QSize(300, 150));
    }

    void resizeDialogSeedsItsFieldsFromTheArgument() {
        ResizeDialog dialog(QSize(800, 600));
        QCOMPARE(dialog.newSize(), QSize(800, 600));
    }

    void videoControlsPlaybackPositionFollowsTheArgument() {
        FloatingWidgetContainer container;
        VideoControls controls(&container);
        controls.setMode(PLAYBACK_VIDEO); // exercise the video branch of the label
        controls.setPlaybackPosition(42);
        QCOMPARE(controls.playbackPosition(), 42);
        // a repeat is the dedup no-op, not a corruption
        controls.setPlaybackPosition(42);
        QCOMPARE(controls.playbackPosition(), 42);
    }

    void cropOverlayImageScaleFollowsTheArgument() {
        FloatingWidgetContainer container;
        CropOverlay overlay(&container);
        overlay.setImageScale(2.5f);
        QCOMPARE(overlay.imageScale(), 2.5f);
    }

    // What a widget destructor forgets cannot be seen by the app: the tree is
    // never torn down, so a leaked child stays reachable from Qt's own widget
    // registry and there is nothing for LSan to call unreachable. Destroying each
    // suspect here is the measurement — the widget class names come from
    // allWidgets(), and the same scopes are what the sanitizer tree uses to size
    // the non-widget heap (a scene, a timeline, a pixmap: not widgets, so not
    // visible to the assertion below and only visible to LSan once unreachable).
    void destroyingAWidgetFreesEveryWidgetItCreated() {
        const QMap<QString, int> before = widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            ImageViewerV2 viewer;
            FolderView folderView;
            FloatingWidgetContainer container;
            ClickZoneOverlay zones(&container);
            ZoomIndicatorOverlay zoom(&container);
            MainPanel panel(&container);
            IconWidget icon;
            // the probe only means something if these really are live widgets
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = widgetClasses();
        QStringList surplus;
        for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if (left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(),
                 qPrintable(QStringLiteral("widgets outliving their parent: ") + surplus.join(", ")));
    }

    // The popup is now owned by the view that shows it, which is what frees it when
    // the view dies. That is only a win if it still behaves like a popup: a free-
    // standing window landing where the button told it, not an embedded child.
    void parentedOptionsPopupStillFloatsWhereItIsTold() {
        QWidget owner;
        owner.resize(200, 100);
        owner.show();
        FVOptionsPopup popup(&owner);
        const QPoint where(120, 90);
        popup.showAt(where);
        QVERIFY2(popup.isWindow(), "parenting turned the popup into an embedded child widget");
        QVERIFY2(popup.windowFlags().testFlag(Qt::Popup), "the popup lost its Popup window flag");
        QVERIFY(popup.isVisible());
        QCOMPARE(popup.pos(), where);
        popup.hide();
        owner.hide();
    }

    // The broad form of this instrument, unlocked by the ownership unification:
    // build a real Core — the full window tree the app never used to tear down —
    // and destroy it, then require the widget census to return exactly to the
    // pre-existing baseline. Until every widget shared_ptr became an observer,
    // this crashed at the first double-free, exactly as the comment it replaces
    // predicted; that prediction is what this test now pins.
    void destroyingTheCoreFreesTheWholeWindowTree() {
        const QMap<QString, int> before = widgetClasses();
        const int beforeTotal = QApplication::allWidgets().size();
        {
            Core core;
            // the probe only means something if the window tree is really live
            QVERIFY(QApplication::allWidgets().size() > beforeTotal);
        }
        const QMap<QString, int> after = widgetClasses();
        QStringList surplus;
        for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
            const int left = it.value() - before.value(it.key(), 0);
            if (left > 0)
                surplus << QStringLiteral("%1 x%2").arg(it.key()).arg(left);
        }
        QVERIFY2(surplus.isEmpty(),
                 qPrintable(QStringLiteral("widgets outliving the destroyed core: ") + surplus.join(", ")));
    }

private:
    static QMap<QString, int> widgetClasses() {
        QMap<QString, int> counts;
        const QList<QWidget *> widgets = QApplication::allWidgets();
        for (QWidget *w : widgets)
            counts[QString::fromUtf8(w->metaObject()->className())]++;
        return counts;
    }

};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestCoreNav t;
    return QTest::qExec(&t, argc, argv);
}

#include "core_nav_tests.moc"

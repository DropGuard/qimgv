#include "engine_stack_fixture.h"

// the declarative settings table: clamps, junk values, and the config isolation
// the binding table stands on
class TestSettingsTable : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestSettingsTable() : EngineStackFixture("settings") {}

private slots:
    void initTestCase() { init(); }

    // Every row of settings_table.h is walked, so a new setting inherits this
    // coverage the moment it is added to the table: missing key -> default,
    // junk value -> default (not QVariant's silent zero/false), clamp bounds
    // hold on both ends.

    void settingsTableRowsSurviveJunkAndClamps() {
        FreshSettings fresh;
        QSettings junk(QCoreApplication::organizationName(), QCoreApplication::applicationName());

        // missing key: every row serves its default
        for(const SettingDesc *d : kAllSettings) {
            switch(d->kind) {
            case SettingDesc::Kind::Bool:
                QCOMPARE(settings->load<bool>(*d), d->defB);
                break;
            case SettingDesc::Kind::Int:
                QCOMPARE(settings->load<qlonglong>(*d), d->defI);
                break;
            case SettingDesc::Kind::Real:
                QCOMPARE(settings->load<double>(*d), d->defR);
                break;
            case SettingDesc::Kind::Str:
                QCOMPARE(settings->load<QString>(*d), QString::fromUtf8(d->defS));
                break;
            }
        }

        // junk string for every row: numbers fall back to the default instead
        // of QVariant's silent zero/false
        for(const SettingDesc *d : kAllSettings)
            junk.setValue(d->key, QVariant(QStringLiteral("banana")));
        junk.sync();
        for(const SettingDesc *d : kAllSettings) {
            switch(d->kind) {
            case SettingDesc::Kind::Bool:
                settings->load<bool>(*d); // defined, do not pin QVariant's whim
                break;
            case SettingDesc::Kind::Int:
                QCOMPARE(settings->load<qlonglong>(*d), d->defI);
                break;
            case SettingDesc::Kind::Real:
                QCOMPARE(settings->load<double>(*d), d->defR);
                break;
            case SettingDesc::Kind::Str:
                QCOMPARE(settings->load<QString>(*d), QStringLiteral("banana"));
                break;
            }
        }

        // clamp bounds hold on both ends, for every row that declares them
        for(const SettingDesc *d : kAllSettings) {
            if(!d->clamped())
                continue;
            if(d->kind == SettingDesc::Kind::Int) {
                if(d->hasLo) {
                    junk.setValue(d->key, QVariant(static_cast<qlonglong>(d->lo) - 1000));
                    junk.sync();
                    QCOMPARE(settings->load<qlonglong>(*d), static_cast<qlonglong>(d->lo));
                }
                if(d->hasHi) {
                    junk.setValue(d->key, QVariant(static_cast<qlonglong>(d->hi) + 1000));
                    junk.sync();
                    QCOMPARE(settings->load<qlonglong>(*d), static_cast<qlonglong>(d->hi));
                }
            } else if(d->kind == SettingDesc::Kind::Real) {
                junk.setValue(d->key, QVariant(d->lo - 1000.0));
                junk.sync();
                QCOMPARE(settings->load<double>(*d), d->lo);
                junk.setValue(d->key, QVariant(d->hi + 1000.0));
                junk.sync();
                QCOMPARE(settings->load<double>(*d), d->hi);
            }
        }
    }

    void settingsEnumGettersClampJunkConfigValues() {
        // a hand-edited config file can hold anything; every enum getter must
        // land on a valid member instead of handing garbage to a switch
        settings->setSortingMode(static_cast<SortingMode>(42));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(static_cast<SortingMode>(-1));
        QVERIFY(settings->sortingMode() == SORT_NAME);
        settings->setSortingMode(SORT_NAME);

        settings->setPanelPosition(static_cast<PanelPosition>(42));
        QVERIFY(settings->panelPosition() == PANEL_RIGHT); // documented fallback
        settings->setPanelPosition(PANEL_TOP);

        // folderEndAction clamping is pinned in its own test; this is the
        // same trap one layer down
        settings->setFolderEndAction(static_cast<FolderEndAction>(99));
        QVERIFY(settings->folderEndAction() == FOLDER_END_NO_ACTION);
        settings->setFolderEndAction(FOLDER_END_NO_ACTION);
    }

    // the state-side lists: bookmarks and saved paths round-trip through the
    // state config (IsolatedHome keeps this off the developer's real state)
    void bookmarksAndSavedPathsRoundTrip() {
        FreshSettings fresh;
        QStringList paths = { "/tmp/a", "/tmp/b with space" };
        settings->setBookmarks(paths);
        settings->setSavedPaths({ "/home/user/pictures" });
        QCOMPARE(settings->bookmarks(), paths);
        QCOMPARE(settings->savedPaths(), QStringList { "/home/user/pictures" });
        settings->setBookmarks({});
        QVERIFY(settings->bookmarks().isEmpty());
    }

    // the open-dialog filter policy: Qt's decodable mime types plus the
    // hardcoded video formats, gated by videoPlayback
    void supportedMimeTypesFollowVideoPlaybackPolicy() {
        FreshSettings fresh;
        settings->setVideoPlayback(true);
        QVERIFY(settings->supportedMimeTypes().contains("video/mp4"));
        QVERIFY(settings->supportedMimeTypes().contains("image/jpeg"));
        settings->setVideoPlayback(false);
        QVERIFY(!settings->supportedMimeTypes().contains("video/mp4"));
        QVERIFY(settings->supportedMimeTypes().contains("image/png"));
    }

    // the shortcut map's round trip through the ini, including the "eq" escape
    // for names that would collide with the k=v encoding
    void shortcutRoundTripEscapesTrailingEquals() {        FreshSettings fresh;
        QMap<QString, QString> shortcuts;
        shortcuts.insert("ctrl+x", "25");
        shortcuts.insert("ctrl+=", "26"); // the "=" key needs the escape
        settings->saveShortcuts(shortcuts);
        QMap<QString, QString> back;
        settings->readShortcuts(back);
        QCOMPARE(back.value("ctrl+x"), QStringLiteral("25"));
        QCOMPARE(back.value("ctrl+="), QStringLiteral("26"));
    }

    void freshSettingsGuardIsolatesConfigState() {
        // without the guard, every test had to reset the keys it depends on
        // and the suite was silently order-sensitive
        settings->setSortingMode(SORT_TIME);
        QVERIFY(settings->sortingMode() == SORT_TIME);
        {
            FreshSettings fresh; // swaps the global to a defaults-only instance
            QVERIFY(settings->sortingMode() == SORT_NAME);
            settings->setSortingMode(SORT_SIZE_DESC);
            QVERIFY(settings->sortingMode() == SORT_SIZE_DESC);
        }
        // the original instance and its in-memory state are back
        QVERIFY(settings->sortingMode() == SORT_TIME);
    }

    // Qt6 killed the @Variant channel for custom types in QSettings: the old
    // Script storage wrote empty shells and read nothing back, silently
    // destroying scripts on every settings save. Scripts now persist as JSON
    // in the same value slot, with a fallback to the pre-@Variant plain keys.
    void scriptsRoundTripAsJsonNotMetatypeShells() {
        FreshSettings fresh;
        QMap<QString, Script> in;
        in.insert(QStringLiteral("Favorite"), Script(QStringLiteral("/usr/local/bin/fav a b %file%"), true));
        settings->saveScripts(in);
        QMap<QString, Script> out;
        settings->readScripts(out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.value("Favorite").command, QStringLiteral("/usr/local/bin/fav a b %file%"));
        QCOMPARE(out.value("Favorite").blocking, true);
        // the stored form must be a JSON string, not a @Variant(...) shell
        QSettings probe(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        probe.beginGroup("Scripts");
        const QVariant raw = probe.value("script/1/value");
        probe.endGroup();
        QCOMPARE(raw.typeId(), QMetaType::QString);
        QVERIFY(raw.toString().startsWith('{'));
    }

    void scriptsFallBackToPreJsonPlainKeys() {
        FreshSettings fresh;
        QSettings conf(QCoreApplication::organizationName(), QCoreApplication::applicationName());
        conf.beginGroup("Scripts");
        conf.beginWriteArray("script");
        conf.setArrayIndex(0);
        conf.setValue("name", QStringLiteral("Favorite"));
        conf.setValue("value", QStringLiteral("@Variant(\\0\\0\\0\\x7f\\0\\0\\0\\aScript\\0)"));
        conf.endArray();
        conf.setValue("1/command", QStringLiteral("/usr/local/bin/fav %file%"));
        conf.endGroup();
        conf.sync();
        QMap<QString, Script> out;
        settings->readScripts(out);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.value("Favorite").command, QStringLiteral("/usr/local/bin/fav %file%"));
    }
};

#include "settings_tests.moc"

int runSettingsTableTests(int argc, char **argv) {
    TestSettingsTable test;
    return runIfOwned(test, argc, argv);
}

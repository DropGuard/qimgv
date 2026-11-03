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
};

#include "settings_tests.moc"

int runSettingsTableTests(int argc, char **argv) {
    TestSettingsTable test;
    return runIfOwned(test, argc, argv);
}

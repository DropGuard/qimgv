#include "engine_stack_fixture.h"

// the seams the test suite itself stands on: bridge instance isolation and the
// extension list the FFI is handed
class TestInfra : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestInfra() : EngineStackFixture("test-infra") {}

private slots:
    void initTestCase() { init(); }

    void bridgeInstancesAreIsolated() {
        // the old singleton forced every test through one engine, one watch
        // target and one session epoch — the per-slot fixture dirs exist only
        // because of that. Two plain instances now get independent engines:
        resetFixture();
        QVERIFY(QDir(fixtureDir).mkdir("other"));
        QFile other(fixtureDir + "/other/z.jpg");
        QVERIFY(other.open(QIODevice::WriteOnly));

        EngineBridge bridgeA;
        EngineBridge bridgeB;
        QSignalSpy listedA(&bridgeA, &EngineBridge::dirListed);
        QSignalSpy listedB(&bridgeB, &EngineBridge::dirListed);
        QVERIFY(listedA.isValid());
        QVERIFY(listedB.isValid());

        QVERIFY(bridgeA.openDir(fixtureDir, 0, false));
        QVERIFY(bridgeB.openDir(fixtureDir + "/other", 0, false));

        QTRY_COMPARE_WITH_TIMEOUT(listedA.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(listedB.count(), 1, 10000);
        QCOMPARE(listedA.at(0).at(0).toString(), fixtureDir);
        QCOMPARE(listedB.at(0).at(0).toString(), fixtureDir + "/other");
        // and the listings do not cross: each instance reports its own engine,
        // including which directory its reads describe
        QCOMPARE(bridgeA.listedDirectory(), fixtureDir);
        QCOMPARE(bridgeB.listedDirectory(), fixtureDir + "/other");
        QCOMPARE(bridgeA.fileCount(), 3);
        QCOMPARE(bridgeB.fileCount(), 1);
    }

    void extensionListForJoinsFormatsAndEmptiesToAcceptAll() {
        // the seam where an empty whitelist once meant "accept nothing"
        // against the header's documented NULL contract
        QCOMPARE(EngineBridge::extensionListFor({QByteArray("jpg"), QByteArray("png")}),
                 QByteArray("jpg\npng"));
        QCOMPARE(EngineBridge::extensionListFor({}), QByteArray());
    }
};

#include "infra_tests.moc"

int runInfraTests(int argc, char **argv) {
    TestInfra test;
    return runIfOwned(test, argc, argv);
}

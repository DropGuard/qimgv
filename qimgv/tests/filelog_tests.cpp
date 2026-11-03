#include "engine_stack_fixture.h"

// the file logging machinery in utils/filelog
class TestFileLog : public QObject, protected EngineStackFixture {
    Q_OBJECT
public:
    TestFileLog() : EngineStackFixture("filelog") {}

private slots:
    void initTestCase() { init(); }

    // install() is thin glue verified by every session's first log lines; the
    // pieces it composes are drivable here with tiny limits.

    void fileLogFormatCarriesLevelCharAndTimestamp() {
        QRegularExpression shape("^\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d+ I hello\\n$");
        QString info = FileLog::formatLine(QtInfoMsg, "hello");
        if(!shape.match(info).hasMatch())
            qWarning() << "formatLine produced:" << info;
        QVERIFY(shape.match(info).hasMatch());
        QCOMPARE(FileLog::formatLine(QtDebugMsg, "x").section(' ', 2, 2), QStringLiteral("D"));
        QCOMPARE(FileLog::formatLine(QtWarningMsg, "x").section(' ', 2, 2), QStringLiteral("W"));
        QCOMPARE(FileLog::formatLine(QtCriticalMsg, "x").section(' ', 2, 2), QStringLiteral("C"));
        QCOMPARE(FileLog::formatLine(QtFatalMsg, "x").section(' ', 2, 2), QStringLiteral("F"));
    }

    void fileLogRotatesPartsAndStopsAtTheCap() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 31 bytes per line (30 'x' + '\n'); the banner is never counted, so
        // the rotation boundary is exactly where the handler's bytes land
        FileLog::SessionLog log(100, 3);
        QVERIFY(log.open(dir.path() + "/qimgv-test.log"));
        const QString line(30, 'x');
        for(int i = 0; i < 3; ++i)
            log.write(line);
        QCOMPARE(log.part(), 1);
        log.write(line);                       // 124 >= 100 -> part 2
        QCOMPARE(log.part(), 2);
        QCOMPARE(log.bytesInPart(), qint64(0));
        for(int i = 0; i < 4; ++i)
            log.write(line);                   // the 4th write rotates -> part 3
        QCOMPARE(log.part(), 3);
        for(int i = 0; i < 4; ++i)
            log.write(line);                   // at the cap: this write lands, then stop
        QVERIFY(!log.isWriting());
        QCOMPARE(QDir(dir.path())
                     .entryList(QStringList() << "qimgv-test.log*", QDir::Files)
                     .size(), 3);
    }

    void fileLogPruneKeepsTheNewestTenSessions() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for(int i = 1; i <= 12; ++i) {
            QFile f(dir.path() + QString("/qimgv-0101-%1.log").arg(i, 2, 10, QChar('0')));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        FileLog::pruneOldSessions(dir.path(), 10);
        QStringList left = QDir(dir.path())
                               .entryList(QStringList() << "qimgv-*.log*", QDir::Files, QDir::Name);
        QCOMPARE(left.size(), 10);
        QCOMPARE(left.first(), QStringLiteral("qimgv-0101-03.log"));
        QCOMPARE(left.last(), QStringLiteral("qimgv-0101-12.log"));
    }
};

#include "filelog_tests.moc"

int runFileLogTests(int argc, char **argv) {
    TestFileLog test;
    return runIfOwned(test, argc, argv);
}

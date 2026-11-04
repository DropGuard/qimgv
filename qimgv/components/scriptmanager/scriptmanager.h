#pragma once

#include <QObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMap>
#include <QDebug>
#include <QStringList>
#include <QProcess>
#include "utils/script.h"
#include "sourcecontainers/image.h"
#include "settings.h"

class ScriptManager : public QObject {
    Q_OBJECT
public:
    static ScriptManager* getInstance();
    ~ScriptManager() override;
    void runScript(const QString &scriptName, std::shared_ptr<Image> img);
    // pure tag substitution over the command line: public static so a test can
    // pin the seam, the way ThumbnailerRunnable::videoFrameTempPath does
    static void processArguments(QStringList &cmd, const std::shared_ptr<Image>& img);
    static QString runCommand(const QString& cmd);
    static void runCommandDetached(const QString& cmd);
    bool scriptExists(const QString& scriptName);
    void readScripts();
    void saveScripts();
    void removeScript(const QString& scriptName);
    const QMap<QString, Script> &allScripts();
    QList<QString> scriptNames();
    Script getScript(const QString& scriptName);
    void addScript(const QString& scriptName, const Script& script);
    static QStringList splitCommandLine(const QString &cmdLine);

signals:
    void error(QString);

private:
    explicit ScriptManager(QObject *parent = nullptr);
    QMap<QString, Script> scripts; // <name, script>

};

extern ScriptManager *scriptManager;

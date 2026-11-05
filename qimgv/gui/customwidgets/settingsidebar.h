#ifndef SETTINGSIDEBAR_H
#define SETTINGSIDEBAR_H

#include <QWidget>
#include <QLabel>
#include <QPainter>
#include <QStyleOption>
#include <QBoxLayout>
#include "gui/customwidgets/iconwidget.h"

class SettingsSidebarItem;

class SettingsSidebar : public QWidget {
    Q_OBJECT
public:
    explicit SettingsSidebar(QWidget *parent = nullptr);
    void addEntry(QString icon, QString name);
    void selectEntry(int idx);

private:
    QBoxLayout *layout;
    QList<SettingsSidebarItem *> entries;
    Qt::Orientation orientation = Qt::Vertical;
    void selectEntryAt(QPoint pos);

signals:
    void entrySelected(int);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    void paintEvent(QPaintEvent *event) override;
};

class SettingsSidebarItem : public QWidget {
    Q_OBJECT
public:
    explicit SettingsSidebarItem(const QString& icon, const QString& name, QWidget *parent = nullptr);
    void setHighlighted(bool mode);
    bool highlighted();

private:
    QBoxLayout *layout;
    IconWidget iconWidget;
    QLabel textLabel;
    bool mHighlighted = false;

protected:
    void paintEvent(QPaintEvent *event) override;
};

#endif // SETTINGSIDEBAR_H

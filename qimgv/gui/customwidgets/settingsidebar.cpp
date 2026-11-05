#include "settingsidebar.h"

#include <utility>

SettingsSidebar::SettingsSidebar(QWidget *parent) : QWidget{parent} {
    layout = new QBoxLayout(QBoxLayout::TopToBottom);
    layout->setSpacing(0);
    layout->setContentsMargins(8,8,9,9);
    layout->addStretch();
    setLayout(layout);
    addEntry(":res/icons/common/settings/general32.png",    tr("General"));
    addEntry(":res/icons/common/settings/view32.png",       tr("View"));
    addEntry(":res/icons/common/settings/appearance32.png", tr("Theme"));
    addEntry(":res/icons/common/settings/shortcuts32.png",  tr("Controls"));
    addEntry(":res/icons/common/settings/terminal32.png",   tr("Scripts"));
    addEntry(":res/icons/common/settings/advanced32.png",   tr("Advanced"));
    addEntry(":res/icons/common/settings/about32.png",      tr("About"));
}

void SettingsSidebar::addEntry(QString icon, QString name) {
    SettingsSidebarItem *entry = new SettingsSidebarItem(std::move(icon), std::move(name));
    layout->insertWidget(entries.count(), entry);
    entries.append(entry);
    if(entries.count() == 1)
        selectEntry(0);
}

void SettingsSidebar::selectEntry(int idx) {
    if(idx >= 0 && idx < entries.count()) {
        foreach(auto entry, entries)
            entry->setHighlighted(false);
        entries[idx]->setHighlighted(true);
        emit entrySelected(idx);
    }
}

void SettingsSidebar::mousePressEvent(QMouseEvent *event) {
    event->accept();
    if(!(event->buttons() & Qt::LeftButton))
        return;
    selectEntryAt(event->pos());

}

void SettingsSidebar::mouseMoveEvent(QMouseEvent *event) {
    event->accept();
    if(!(event->buttons() & Qt::LeftButton))
        return;
    selectEntryAt(event->pos());
}

void SettingsSidebar::selectEntryAt(QPoint pos) {
    int newSelection = -1;
    for(int i = 0; i < entries.count(); i++) {
        if(entries[i]->geometry().contains(pos)) {
            if(!entries[i]->highlighted())
                newSelection = i;
            break;
        }
    }
    selectEntry(newSelection);
}

void SettingsSidebar::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

// -------------------------------------------------------------------

SettingsSidebarItem::SettingsSidebarItem(const QString& icon, const QString& name, QWidget *parent) : QWidget{parent} {
    QPalette p;
    if(p.base().color().valueF() <= 0.45f)
        iconWidget.setColor(QColor(184,184,185));
    else
        iconWidget.setColor(QColor(70,70,70));
    iconWidget.setIconPath(icon);
    textLabel.setText(name);
    layout = new QBoxLayout(QBoxLayout::LeftToRight);
    layout->setContentsMargins(6,4,6,4);
    layout->setSpacing(7);
    layout->addWidget(&iconWidget);
    layout->addWidget(&textLabel);
    layout->addStretch();
    setLayout(layout);
    setMouseTracking(true);
}

void SettingsSidebarItem::setHighlighted(bool mode) {
    mHighlighted = mode;
    this->setProperty("checked", mHighlighted);
    style()->unpolish(this);
    style()->polish(this);
}

bool SettingsSidebarItem::highlighted() {
    return mHighlighted;
}

void SettingsSidebarItem::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

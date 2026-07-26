#ifndef TRAY_MANAGER_H
#define TRAY_MANAGER_H

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QIcon>

enum class TrayStatusState {
    Offline,
    ClientOnly,
    WorkerActive
};

class TrayManager : public QObject {
    Q_OBJECT

public:
    explicit TrayManager(QObject *parent = nullptr);
    void setupTray();
    void updateState(TrayStatusState state);
    bool isSystemTrayAvailable() const { return QSystemTrayIcon::isSystemTrayAvailable(); }

signals:
    void showMainWindowRequested();
    void toggleWorkerRequested();
    void openDashboardRequested();
    void quitRequested();

private slots:
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);

private:
    QIcon createColoredIcon(const QColor &color);

    QSystemTrayIcon m_trayIcon;
    QMenu m_trayMenu;
    QAction *m_toggleWorkerAction = nullptr;
    TrayStatusState m_currentState = TrayStatusState::Offline;
};

#endif // TRAY_MANAGER_H

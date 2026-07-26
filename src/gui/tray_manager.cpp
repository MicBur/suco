#include "tray_manager.h"
#include <QPixmap>
#include <QPainter>

TrayManager::TrayManager(QObject *parent)
    : QObject(parent) {}

void TrayManager::setupTray() {
    QAction *showAction = m_trayMenu.addAction("Open Control Center");
    connect(showAction, &QAction::triggered, this, &TrayManager::showMainWindowRequested);

    m_toggleWorkerAction = m_trayMenu.addAction("Toggle Worker Mode");
    connect(m_toggleWorkerAction, &QAction::triggered, this, &TrayManager::toggleWorkerRequested);

    QAction *dashAction = m_trayMenu.addAction("Open Web Dashboard");
    connect(dashAction, &QAction::triggered, this, &TrayManager::openDashboardRequested);

    m_trayMenu.addSeparator();

    QAction *quitAction = m_trayMenu.addAction("Exit");
    connect(quitAction, &QAction::triggered, this, &TrayManager::quitRequested);

    m_trayIcon.setContextMenu(&m_trayMenu);
    connect(&m_trayIcon, &QSystemTrayIcon::activated, this, &TrayManager::onTrayIconActivated);

    updateState(TrayStatusState::Offline);
    m_trayIcon.show();
}

void TrayManager::updateState(TrayStatusState state) {
    m_currentState = state;
    QColor color;
    QString tooltip;

    switch (state) {
    case TrayStatusState::WorkerActive:
        color = QColor("#10B981"); // Emerald green
        tooltip = "SUCO Grid: WIN-DEV Worker Active (Sharing Slots)";
        if (m_toggleWorkerAction) m_toggleWorkerAction->setText("Stop Worker Mode");
        break;
    case TrayStatusState::ClientOnly:
        color = QColor("#3B82F6"); // Neon Blue
        tooltip = "SUCO Grid: Client Mode (Connected)";
        if (m_toggleWorkerAction) m_toggleWorkerAction->setText("Start Worker Mode");
        break;
    case TrayStatusState::Offline:
    default:
        color = QColor("#EF4444"); // Red
        tooltip = "SUCO Grid: Coordinator Offline";
        if (m_toggleWorkerAction) m_toggleWorkerAction->setText("Start Worker Mode");
        break;
    }

    m_trayIcon.setIcon(createColoredIcon(color));
    m_trayIcon.setToolTip(tooltip);
}

void TrayManager::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        emit showMainWindowRequested();
    }
}

QIcon TrayManager::createColoredIcon(const QColor &color) {
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw outer glow circle
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color.red(), color.green(), color.blue(), 80));
    painter.drawEllipse(2, 2, 28, 28);

    // Draw solid inner circle
    painter.setBrush(color);
    painter.drawEllipse(6, 6, 20, 20);

    return QIcon(pixmap);
}

#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QCloseEvent>

#include "worker_manager.h"
#include "grid_client.h"
#include "tray_manager.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onToggleWorkerClicked();
    void onOpenDashboardClicked();
    void onWorkerStateChanged(bool active);
    void onWorkerLogReceived(const QString &msg);
    void onConnectionStatusChanged(bool online, const QString &statusText);
    void onGridStatsUpdated(int totalRequests, double cacheHitRate, int activeJobs, int totalWorkers, int totalSlots, int slotsUsed);
    void onWorkerListUpdated(const QList<WorkerInfo> &workers);

private:
    void setupUi();
    void applyDarkTheme();

    WorkerManager m_workerManager;
    GridClient m_gridClient;
    TrayManager m_trayManager;

    // UI Widgets
    QLabel *m_statusDotLabel = nullptr;
    QLabel *m_statusTextLabel = nullptr;
    QLineEdit *m_coordinatorEdit = nullptr;

    QPushButton *m_workerToggleButton = nullptr;
    QSpinBox *m_slotsSpinBox = nullptr;

    QLabel *m_valWorkersCard = nullptr;
    QLabel *m_valSlotsCard = nullptr;
    QLabel *m_valCacheHitCard = nullptr;

    QTableWidget *m_workerTable = nullptr;
    QTextEdit *m_logConsole = nullptr;

    bool m_isOnline = false;
};

#endif // MAIN_WINDOW_H

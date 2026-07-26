#include "main_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setupUi();
    applyDarkTheme();

    m_trayManager.setupTray();

    // Tray signals
    connect(&m_trayManager, &TrayManager::showMainWindowRequested, this, [this]() {
        showNormal();
        activateWindow();
    });
    connect(&m_trayManager, &TrayManager::toggleWorkerRequested, this, &MainWindow::onToggleWorkerClicked);
    connect(&m_trayManager, &TrayManager::openDashboardRequested, this, &MainWindow::onOpenDashboardClicked);
    connect(&m_trayManager, &TrayManager::quitRequested, qApp, &QCoreApplication::quit);

    // Worker signals
    connect(&m_workerManager, &WorkerManager::workerStateChanged, this, &MainWindow::onWorkerStateChanged);
    connect(&m_workerManager, &WorkerManager::workerLogReceived, this, &MainWindow::onWorkerLogReceived);

    // Grid Client signals
    connect(&m_gridClient, &GridClient::connectionStatusChanged, this, &MainWindow::onConnectionStatusChanged);
    connect(&m_gridClient, &GridClient::gridStatsUpdated, this, &MainWindow::onGridStatsUpdated);
    connect(&m_gridClient, &GridClient::workerListUpdated, this, &MainWindow::onWorkerListUpdated);

    // Coordinator IP change
    connect(m_coordinatorEdit, &QLineEdit::returnPressed, [this]() {
        m_gridClient.setCoordinatorHost(m_coordinatorEdit->text().trimmed());
    });

    m_gridClient.startPolling(2000);
}

MainWindow::~MainWindow() {}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_trayManager.isSystemTrayAvailable()) {
        hide();
        event->ignore();
    } else {
        event->accept();
    }
}

void MainWindow::setupUi() {
    setWindowTitle("SUCO Control Center (Grid & Worker Manager)");
    resize(850, 620);

    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // --- Header Section ---
    QHBoxLayout *headerLayout = new QHBoxLayout();
    QLabel *titleLabel = new QLabel("⚡ SUCO Grid Control Center", this);
    titleLabel->setStyleSheet("font-size: 20px; font-weight: bold; color: #F8FAFC;");

    m_statusDotLabel = new QLabel("●", this);
    m_statusDotLabel->setStyleSheet("font-size: 16px; color: #EF4444;");
    m_statusTextLabel = new QLabel("Connecting...", this);
    m_statusTextLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #94A3B8;");

    m_coordinatorEdit = new QLineEdit("192.168.0.200", this);
    m_coordinatorEdit->setFixedWidth(130);
    m_coordinatorEdit->setToolTip("Coordinator IP (Press Enter to update)");

    QPushButton *dashBtn = new QPushButton("🌐 Open Web Dashboard", this);
    dashBtn->setCursor(Qt::PointingHandCursor);
    connect(dashBtn, &QPushButton::clicked, this, &MainWindow::onOpenDashboardClicked);

    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_statusDotLabel);
    headerLayout->addWidget(m_statusTextLabel);
    headerLayout->addWidget(new QLabel("IP:", this));
    headerLayout->addWidget(m_coordinatorEdit);
    headerLayout->addWidget(dashBtn);

    mainLayout->addLayout(headerLayout);

    // --- Worker Toggle Card ---
    QGroupBox *toggleBox = new QGroupBox("Local Worker Mode (WIN-DEV)", this);
    QHBoxLayout *toggleLayout = new QHBoxLayout(toggleBox);
    toggleLayout->setContentsMargins(16, 16, 16, 16);

    QLabel *toggleDesc = new QLabel("Share local 24 CPU cores with the SUCO compiler grid to speed up builds across all machines.", this);
    toggleDesc->setWordWrap(true);
    toggleDesc->setStyleSheet("color: #94A3B8; font-size: 13px;");

    QLabel *slotsLabel = new QLabel("Slots:", this);
    slotsLabel->setStyleSheet("color: #E2E8F0; font-weight: bold;");

    m_slotsSpinBox = new QSpinBox(this);
    m_slotsSpinBox->setRange(1, 32);
    m_slotsSpinBox->setValue(8);
    m_slotsSpinBox->setFixedWidth(60);

    m_workerToggleButton = new QPushButton("▶ Start WIN-DEV Worker", this);
    m_workerToggleButton->setFixedHeight(40);
    m_workerToggleButton->setCursor(Qt::PointingHandCursor);
    m_workerToggleButton->setStyleSheet("background-color: #10B981; color: #FFFFFF; font-size: 14px; font-weight: bold; border-radius: 6px; padding: 0 16px;");
    connect(m_workerToggleButton, &QPushButton::clicked, this, &MainWindow::onToggleWorkerClicked);

    toggleLayout->addWidget(toggleDesc, 1);
    toggleLayout->addWidget(slotsLabel);
    toggleLayout->addWidget(m_slotsSpinBox);
    toggleLayout->addSpacing(12);
    toggleLayout->addWidget(m_workerToggleButton);

    mainLayout->addWidget(toggleBox);

    // --- Stat Cards Grid ---
    QGridLayout *cardsLayout = new QGridLayout();

    auto createCard = [this](const QString &title, QLabel *&valLabel, const QString &defaultVal, const QString &accentColor) {
        QGroupBox *card = new QGroupBox(title, this);
        QVBoxLayout *cLayout = new QVBoxLayout(card);
        cLayout->setContentsMargins(12, 12, 12, 12);
        valLabel = new QLabel(defaultVal, card);
        valLabel->setStyleSheet(QString("font-size: 24px; font-weight: bold; color: %1;").arg(accentColor));
        valLabel->setAlignment(Qt::AlignCenter);
        cLayout->addWidget(valLabel);
        return card;
    };

    QGroupBox *card1 = createCard("Active Grid Workers", m_valWorkersCard, "0 Workers", "#3B82F6");
    QGroupBox *card2 = createCard("Total Available Slots", m_valSlotsCard, "0 Slots", "#10B981");
    QGroupBox *card3 = createCard("Grid Cache Hit Rate", m_valCacheHitCard, "0.0 %", "#8B5CF6");

    cardsLayout->addWidget(card1, 0, 0);
    cardsLayout->addWidget(card2, 0, 1);
    cardsLayout->addWidget(card3, 0, 2);

    mainLayout->addLayout(cardsLayout);

    // --- Worker Node Table ---
    m_workerTable = new QTableWidget(0, 4, this);
    m_workerTable->setHorizontalHeaderLabels(QStringList() << "Worker Node" << "IP Address" << "Target OS" << "Slots (Used / Total)");
    m_workerTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_workerTable->verticalHeader()->setVisible(false);
    m_workerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_workerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_workerTable->setFixedHeight(150);

    mainLayout->addWidget(m_workerTable);

    // --- Log Console ---
    m_logConsole = new QTextEdit(this);
    m_logConsole->setReadOnly(true);
    m_logConsole->setFixedHeight(100);
    m_logConsole->setPlaceholderText("SUCO Control Center activity log...");

    mainLayout->addWidget(m_logConsole);

    setCentralWidget(centralWidget);
}

void MainWindow::applyDarkTheme() {
    QString qss = R"(
        QMainWindow {
            background-color: #0F172A;
        }
        QWidget {
            color: #E2E8F0;
            font-family: 'Segoe UI', Roboto, sans-serif;
        }
        QGroupBox {
            font-weight: bold;
            font-size: 13px;
            border: 1px solid #334155;
            border-radius: 8px;
            margin-top: 6px;
            padding-top: 10px;
            background-color: #1E293B;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px;
            color: #38BDF8;
        }
        QLineEdit, QSpinBox {
            background-color: #0F172A;
            border: 1px solid #475569;
            border-radius: 4px;
            padding: 6px;
            color: #F8FAFC;
        }
        QPushButton {
            background-color: #334155;
            color: #F8FAFC;
            border: none;
            border-radius: 4px;
            padding: 6px 12px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #475569;
        }
        QTableWidget {
            background-color: #1E293B;
            border: 1px solid #334155;
            border-radius: 6px;
            gridline-color: #334155;
        }
        QHeaderView::section {
            background-color: #0F172A;
            color: #94A3B8;
            font-weight: bold;
            border: none;
            padding: 6px;
        }
        QTextEdit {
            background-color: #0F172A;
            border: 1px solid #334155;
            border-radius: 6px;
            color: #38BDF8;
            font-family: 'Consolas', 'Courier New', monospace;
            font-size: 12px;
        }
    )";
    setStyleSheet(qss);
}

void MainWindow::onToggleWorkerClicked() {
    if (m_workerManager.isRunning()) {
        m_workerManager.stopWorker();
    } else {
        m_workerManager.startWorker(m_coordinatorEdit->text().trimmed(), m_slotsSpinBox->value());
    }
}

void MainWindow::onOpenDashboardClicked() {
    QString url = QString("http://%1:9001/").arg(m_coordinatorEdit->text().trimmed());
    QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::onWorkerStateChanged(bool active) {
    if (active) {
        m_workerToggleButton->setText("■ Stop WIN-DEV Worker");
        m_workerToggleButton->setStyleSheet("background-color: #EF4444; color: #FFFFFF; font-size: 14px; font-weight: bold; border-radius: 6px; padding: 0 16px;");
        m_slotsSpinBox->setEnabled(false);
        m_trayManager.updateState(TrayStatusState::WorkerActive);
    } else {
        m_workerToggleButton->setText("▶ Start WIN-DEV Worker");
        m_workerToggleButton->setStyleSheet("background-color: #10B981; color: #FFFFFF; font-size: 14px; font-weight: bold; border-radius: 6px; padding: 0 16px;");
        m_slotsSpinBox->setEnabled(true);
        m_trayManager.updateState(m_isOnline ? TrayStatusState::ClientOnly : TrayStatusState::Offline);
    }
}

void MainWindow::onWorkerLogReceived(const QString &msg) {
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    m_logConsole->append(QString("[%1] %2").arg(timeStr, msg));
}

void MainWindow::onConnectionStatusChanged(bool online, const QString &statusText) {
    m_isOnline = online;
    if (online) {
        m_statusDotLabel->setStyleSheet("font-size: 16px; color: #10B981;");
        m_statusTextLabel->setText("Online");
        m_statusTextLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #10B981;");
        if (!m_workerManager.isRunning()) {
            m_trayManager.updateState(TrayStatusState::ClientOnly);
        }
    } else {
        m_statusDotLabel->setStyleSheet("font-size: 16px; color: #EF4444;");
        m_statusTextLabel->setText(statusText);
        m_statusTextLabel->setStyleSheet("font-size: 13px; font-weight: bold; color: #EF4444;");
        if (!m_workerManager.isRunning()) {
            m_trayManager.updateState(TrayStatusState::Offline);
        }
    }
}

void MainWindow::onGridStatsUpdated(int totalRequests, double cacheHitRate, int activeJobs, int totalWorkers, int totalSlots, int slotsUsed) {
    Q_UNUSED(totalRequests);
    Q_UNUSED(activeJobs);
    m_valWorkersCard->setText(QString("%1 Workers").arg(totalWorkers));
    m_valSlotsCard->setText(QString("%1 / %2 Slots").arg(slotsUsed).arg(totalSlots));
    m_valCacheHitCard->setText(QString("%1 %").arg(QString::number(cacheHitRate, 'f', 1)));
}

void MainWindow::onWorkerListUpdated(const QList<WorkerInfo> &workers) {
    m_workerTable->setRowCount(workers.size());
    for (int i = 0; i < workers.size(); ++i) {
        const WorkerInfo &w = workers[i];
        QTableWidgetItem *nameItem = new QTableWidgetItem(w.name);
        QTableWidgetItem *ipItem = new QTableWidgetItem(w.ip);

        QString osBadge = w.os.toLower().contains("win") ? "⊞ Windows" : "🐧 Linux";
        QTableWidgetItem *osItem = new QTableWidgetItem(osBadge);
        if (w.os.toLower().contains("win")) {
            osItem->setForeground(QColor("#38BDF8"));
        } else {
            osItem->setForeground(QColor("#10B981"));
        }

        QTableWidgetItem *slotsItem = new QTableWidgetItem(QString("%1 / %2").arg(w.slotsUsed).arg(w.slotsTotal));

        m_workerTable->setItem(i, 0, nameItem);
        m_workerTable->setItem(i, 1, ipItem);
        m_workerTable->setItem(i, 2, osItem);
        m_workerTable->setItem(i, 3, slotsItem);
    }
}

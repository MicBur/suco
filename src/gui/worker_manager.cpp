#include "worker_manager.h"
#include <QCoreApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QDebug>

WorkerManager::WorkerManager(QObject *parent)
    : QObject(parent) {
    connect(&m_process, &QProcess::started, this, &WorkerManager::onProcessStarted);
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &WorkerManager::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &WorkerManager::onProcessError);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &WorkerManager::onReadyReadStandardOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &WorkerManager::onReadyReadStandardError);
}

WorkerManager::~WorkerManager() {
    stopWorker();
}

bool WorkerManager::isRunning() const {
    return m_active && m_process.state() == QProcess::Running;
}

void WorkerManager::startWorker(const QString &coordinatorHost, int slotCount) {
    if (isRunning()) return;

    QString appDir = QCoreApplication::applicationDirPath();
    QString workerExec = appDir + "/suco-worker.exe";
    if (!QFile::exists(workerExec)) {
        workerExec = appDir + "/suco-worker";
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Ensure SUCO_SECRET is populated from user env if not set in process env
    if (env.value("SUCO_SECRET").isEmpty()) {
        QString userSecret = qgetenv("SUCO_SECRET");
        if (!userSecret.isEmpty()) {
            env.insert("SUCO_SECRET", userSecret);
        }
    }
    m_process.setProcessEnvironment(env);

    QStringList args;
    args << "--coordinator" << (coordinatorHost + ":9000")
         << "--slots" << QString::number(slotCount);

    emit workerLogReceived(QString("Starting worker: %1 %2").arg(workerExec, args.join(" ")));
    m_process.start(workerExec, args);
}

void WorkerManager::stopWorker() {
    if (m_process.state() != QProcess::NotRunning) {
        emit workerLogReceived("Stopping worker process...");
        m_process.terminate();
        if (!m_process.waitForFinished(3000)) {
            m_process.kill();
        }
    }
    m_active = false;
    emit workerStateChanged(false);
}

void WorkerManager::onProcessStarted() {
    m_active = true;
    emit workerLogReceived("Worker process started successfully.");
    emit workerStateChanged(true);
}

void WorkerManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    m_active = false;
    emit workerLogReceived(QString("Worker process finished with exit code %1.").arg(exitCode));
    emit workerStateChanged(false);
}

void WorkerManager::onProcessError(QProcess::ProcessError error) {
    m_active = false;
    QString errStr = QString("Worker process error: %1").arg(m_process.errorString());
    emit workerErrorReceived(errStr);
    emit workerStateChanged(false);
}

void WorkerManager::onReadyReadStandardOutput() {
    QString out = QString::fromUtf8(m_process.readAllStandardOutput()).trimmed();
    if (!out.isEmpty()) {
        emit workerLogReceived(out);
    }
}

void WorkerManager::onReadyReadStandardError() {
    QString err = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
    if (!err.isEmpty()) {
        emit workerLogReceived(err);
    }
}

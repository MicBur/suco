#ifndef WORKER_MANAGER_H
#define WORKER_MANAGER_H

#include <QObject>
#include <QProcess>
#include <QString>

class WorkerManager : public QObject {
    Q_OBJECT

public:
    explicit WorkerManager(QObject *parent = nullptr);
    ~WorkerManager();

    bool isRunning() const;
    void startWorker(const QString &coordinatorHost, int slotCount);
    void stopWorker();

signals:
    void workerStateChanged(bool active);
    void workerLogReceived(const QString &message);
    void workerErrorReceived(const QString &error);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();

private:
    QProcess m_process;
    bool m_active = false;
};

#endif // WORKER_MANAGER_H

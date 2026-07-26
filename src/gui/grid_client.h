#ifndef GRID_CLIENT_H
#define GRID_CLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>

struct WorkerInfo {
    QString name;
    QString ip;
    QString os;
    int slotsTotal;
    int slotsUsed;
};

class GridClient : public QObject {
    Q_OBJECT

public:
    explicit GridClient(QObject *parent = nullptr);
    void setCoordinatorHost(const QString &host);
    QString coordinatorHost() const { return m_host; }

    void startPolling(int intervalMs = 2000);
    void stopPolling();

signals:
    void gridStatsUpdated(int totalRequests, double cacheHitRate, int activeJobs, int totalWorkers, int totalSlots, int slotsUsed);
    void workerListUpdated(const QList<WorkerInfo> &workers);
    void connectionStatusChanged(bool online, const QString &statusText);

private slots:
    void fetchStats();
    void onStatsReplyFinished(QNetworkReply *reply);

private:
    QNetworkAccessManager m_networkManager;
    QTimer m_pollTimer;
    QString m_host = "192.168.0.200";
    bool m_online = false;
};

#endif // GRID_CLIENT_H

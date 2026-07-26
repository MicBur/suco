#include "grid_client.h"
#include <QJsonDocument>
#include <QUrl>
#include <QNetworkRequest>

GridClient::GridClient(QObject *parent)
    : QObject(parent) {
    connect(&m_pollTimer, &QTimer::timeout, this, &GridClient::fetchStats);
}

void GridClient::setCoordinatorHost(const QString &host) {
    m_host = host;
}

void GridClient::startPolling(int intervalMs) {
    m_pollTimer.start(intervalMs);
    fetchStats();
}

void GridClient::stopPolling() {
    m_pollTimer.stop();
}

void GridClient::fetchStats() {
    QString urlStr = QString("http://%1:9001/api/stats").arg(m_host);
    QNetworkRequest request((QUrl(urlStr)));
    request.setTransferTimeout(1500);

    QNetworkReply *reply = m_networkManager.get(request);
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        onStatsReplyFinished(reply);
        reply->deleteLater();
    });
}

void GridClient::onStatsReplyFinished(QNetworkReply *reply) {
    if (reply->error() != QNetworkReply::NoError) {
        if (m_online) {
            m_online = false;
            emit connectionStatusChanged(false, QString("Offline (%1)").arg(reply->errorString()));
        }
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    int totalRequests = obj.value("total_requests").toInt(0);
    double cacheHitRate = obj.value("cache_hit_rate").toDouble(0.0);
    int activeJobs = obj.value("active_jobs").toInt(0);

    QJsonArray workersArr = obj.value("workers").toArray();
    QList<WorkerInfo> workerList;
    int totalSlots = 0;
    int slotsUsed = 0;

    for (const QJsonValue &val : workersArr) {
        QJsonObject wObj = val.toObject();
        WorkerInfo info;
        info.name = wObj.value("name").toString();
        info.ip = wObj.value("ip").toString();
        info.os = wObj.value("os").toString();
        info.slotsTotal = wObj.value("slots_total").toInt(0);
        info.slotsUsed = wObj.value("slots_used").toInt(0);

        totalSlots += info.slotsTotal;
        slotsUsed += info.slotsUsed;
        workerList.append(info);
    }

    if (!m_online) {
        m_online = true;
        emit connectionStatusChanged(true, "Online");
    }

    emit gridStatsUpdated(totalRequests, cacheHitRate, activeJobs, workerList.size(), totalSlots, slotsUsed);
    emit workerListUpdated(workerList);
}

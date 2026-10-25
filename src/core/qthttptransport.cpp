/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qthttptransport.h"

#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace Chomikuj
{

namespace
{

QNetworkRequest makeRequest(const HttpRequest &request, bool streaming)
{
    QNetworkRequest networkRequest(request.url);
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                               QNetworkRequest::NoLessSafeRedirectPolicy);

    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        networkRequest.setRawHeader(it.key(), it.value());
    }

    if (streaming && !networkRequest.hasRawHeader("Accept-Encoding")) {
        // Keep Content-Length meaningful for the download progress.
        networkRequest.setRawHeader("Accept-Encoding", "identity");
    }

    return networkRequest;
}

HttpResponse responseFromReply(QNetworkReply *reply, const QByteArray &body, const QString &errorString)
{
    HttpResponse response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = body;
    response.errorString = errorString;

    const auto pairs = reply->rawHeaderPairs();
    for (const auto &pair : pairs) {
        response.headers.insert(pair.first, pair.second);
    }

    if (response.status == 0 && response.errorString.isEmpty() && reply->error() != QNetworkReply::NoError) {
        response.errorString = reply->errorString();
    }
    return response;
}

} // namespace

QtHttpTransport::QtHttpTransport() = default;

HttpResponse QtHttpTransport::send(const HttpRequest &request)
{
    QNetworkRequest networkRequest = makeRequest(request, false);
    QNetworkReply *reply = nullptr;
    if (request.body.isEmpty() && request.method == QByteArrayLiteral("GET")) {
        reply = m_manager.get(networkRequest);
    } else if (request.body.isEmpty() && request.method == QByteArrayLiteral("HEAD")) {
        // Qt 6.11 can keep a sendCustomRequest("HEAD") pending until timeout.
        reply = m_manager.head(networkRequest);
    } else if (request.body.isEmpty() && request.method == QByteArrayLiteral("DELETE")) {
        reply = m_manager.deleteResource(networkRequest);
    } else {
        reply = m_manager.sendCustomRequest(networkRequest, request.method, request.body);
    }

    QEventLoop loop;
    QTimer timer;
    QTimer cancelTimer;
    bool timedOut = false;
    bool cancelledByUser = false;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, reply, [&] {
        timedOut = true;
        reply->abort();
    });
    if (request.cancelled) {
        cancelTimer.setInterval(200);
        QObject::connect(&cancelTimer, &QTimer::timeout, reply, [&] {
            if (request.cancelled()) {
                cancelledByUser = true;
                reply->abort();
            }
        });
        cancelTimer.start();
    }
    if (request.timeoutMs > 0) {
        timer.start(request.timeoutMs);
    }
    loop.exec();
    cancelTimer.stop();

    const QByteArray body = reply->readAll();
    QString errorString;
    if (cancelledByUser) {
        errorString = QStringLiteral("cancelled");
    } else if (timedOut) {
        errorString = QStringLiteral("request timed out");
    } else if (reply->error() != QNetworkReply::NoError
               && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
        errorString = reply->errorString();
    }

    HttpResponse response = responseFromReply(reply, body, errorString);
    response.requestBodySent = reply->bytesToWrite() == 0;
    delete reply;
    return response;
}

HttpResponse QtHttpTransport::sendStreaming(const HttpRequest &request,
                                            const DataCallback &onData,
                                            const ProgressCallback &onProgress)
{
    QNetworkRequest networkRequest = makeRequest(request, true);
    QNetworkReply *reply = m_manager.get(networkRequest);

    QEventLoop loop;
    QTimer timer;
    QTimer cancelTimer;
    bool timedOut = false;
    bool abortedByCallback = false;
    bool cancelledByUser = false;
    qint64 received = 0;

    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, reply, [&] {
        timedOut = true;
        reply->abort();
    });
    if (request.cancelled) {
        cancelTimer.setInterval(200);
        QObject::connect(&cancelTimer, &QTimer::timeout, reply, [&] {
            if (request.cancelled()) {
                cancelledByUser = true;
                reply->abort();
            }
        });
        cancelTimer.start();
    }

    auto restartTimer = [&] {
        if (request.timeoutMs > 0) {
            timer.start(request.timeoutMs);
        }
    };

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        const QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            return;
        }
        received += data.size();
        if (onData && !onData(data)) {
            abortedByCallback = true;
            reply->abort();
            return;
        }
        if (onProgress) {
            const QByteArray length = reply->rawHeader("Content-Length");
            const qint64 total = length.isEmpty() ? -1 : length.toLongLong();
            onProgress(received, total);
        }
        restartTimer();
    });

    restartTimer();
    loop.exec();
    cancelTimer.stop();

    const QByteArray body = reply->readAll();
    if (!body.isEmpty() && !abortedByCallback) {
        received += body.size();
        if (onData && !onData(body)) {
            abortedByCallback = true;
        }
        if (onProgress) {
            const QByteArray length = reply->rawHeader("Content-Length");
            const qint64 total = length.isEmpty() ? -1 : length.toLongLong();
            onProgress(received, total);
        }
    }

    QString errorString;
    if (cancelledByUser) {
        errorString = QStringLiteral("cancelled");
    } else if (timedOut) {
        errorString = QStringLiteral("request timed out");
    } else if (abortedByCallback) {
        errorString = QStringLiteral("cancelled");
    } else if (reply->error() != QNetworkReply::NoError
               && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
        errorString = reply->errorString();
    }

    HttpResponse response = responseFromReply(reply, {}, errorString);
    response.requestBodySent = true;
    delete reply;
    return response;
}

} // namespace Chomikuj

/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rangereader.h"

#include "chomikujerror.h"
#include "retry.h"

#include <KIO/Global>

#include <QRegularExpression>
#include <QUrl>

#include <limits>
#include <utility>

namespace Chomikuj
{

namespace
{

constexpr int s_timeoutMs = 60000;
constexpr int s_maxNetworkRetries = 1;

bool isRefreshStatus(int status)
{
    return status == 401 || status == 403 || status == 404 || status == 410;
}

bool isRetryableStatus(int status)
{
    return status == 408 || status == 429 || status == 500
        || status == 502 || status == 503 || status == 504;
}

struct ContentRange
{
    qint64 start = -1;
    qint64 end = -1;
    qint64 total = -1;
};

ContentRange parseContentRange(const QByteArray &header)
{
    ContentRange result;
    if (header.isEmpty()) {
        return result;
    }

    static const QRegularExpression withRange(
        QStringLiteral("^\\s*bytes\\s+(\\d+)-(\\d+)/(\\d+|\\*)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression totalOnly(
        QStringLiteral("^\\s*bytes\\s+\\*/(\\d+|\\*)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);

    const QString text = QString::fromLatin1(header).trimmed();
    QRegularExpressionMatch match = withRange.match(text);
    if (match.hasMatch()) {
        bool startOk = false;
        bool endOk = false;
        result.start = match.captured(1).toLongLong(&startOk);
        result.end = match.captured(2).toLongLong(&endOk);
        if (!startOk || !endOk) {
            return {};
        }
        if (match.captured(3) != QLatin1String("*")) {
            bool totalOk = false;
            result.total = match.captured(3).toLongLong(&totalOk);
            if (!totalOk) {
                result.total = -1;
            }
        }
        return result;
    }

    match = totalOnly.match(text);
    if (match.hasMatch() && match.captured(1) != QLatin1String("*")) {
        bool totalOk = false;
        result.total = match.captured(1).toLongLong(&totalOk);
        if (!totalOk) {
            result.total = -1;
        }
    }
    return result;
}

qint64 contentLength(const HttpResponse &response)
{
    bool ok = false;
    const qint64 length = response.header("Content-Length").toLongLong(&ok);
    return ok ? length : -1;
}

bool isCancelled(const RangeReader::Cancelled &cancelled)
{
    return cancelled && cancelled();
}

void throwCancelled()
{
    throw Error(KIO::ERR_USER_CANCELED, QStringLiteral("Download cancelled"));
}

void retryOrThrow(int &retries, const RangeReader::Cancelled &cancelled)
{
    if (retries++ >= s_maxNetworkRetries) {
        throw Error(KIO::ERR_CONNECTION_BROKEN,
                    QStringLiteral("Chomikuj range download failed"));
    }
    sleepInterruptible(retryDelayMs(retries), cancelled);
    if (isCancelled(cancelled)) {
        throwCancelled();
    }
}

} // namespace

RangeReader::RangeReader(HttpTransport &transport, QString userAgent, QString url,
                         qint64 fallbackSize, qint64 knownSize)
    : m_transport(transport)
    , m_userAgent(std::move(userAgent))
    , m_url(std::move(url))
    , m_size(knownSize >= 0 ? knownSize : qMax<qint64>(0, fallbackSize))
    , m_sizeKnown(knownSize >= 0)
{
}

qint64 RangeReader::size() const
{
    return m_size;
}

bool RangeReader::sizeKnown() const
{
    return m_sizeKnown;
}

QString RangeReader::url() const
{
    return m_url;
}

qint64 RangeReader::probeExactSize(const UrlRefresher &refreshUrl,
                                   const Cancelled &cancelled)
{
    if (m_sizeKnown || m_url.isEmpty()) {
        return m_size;
    }

    bool refreshed = false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (isCancelled(cancelled)) {
            throwCancelled();
        }

        HttpRequest headRequest;
        headRequest.method = QByteArrayLiteral("HEAD");
        headRequest.url = QUrl(m_url);
        headRequest.timeoutMs = 30000;
        headRequest.cancelled = cancelled;
        headRequest.headers.insert("User-Agent", m_userAgent.toUtf8());
        headRequest.headers.insert("Accept-Encoding", "identity");

        const HttpResponse head = m_transport.send(headRequest);

        if (isRefreshStatus(head.status) && !refreshed && refreshUrl) {
            const QString next = refreshUrl();
            if (!next.isEmpty()) {
                m_url = next;
                refreshed = true;
                continue;
            }
        }

        if (head.status >= 200 && head.status < 300) {
            const qint64 length = contentLength(head);
            if (length >= 0) {
                m_size = length;
                m_sizeKnown = true;
                return m_size;
            }
        }
        break;
    }

    if (isCancelled(cancelled)) {
        throwCancelled();
    }

    // Not every signed URL answers HEAD.  Ask for one byte and derive the
    // total from Content-Range (or Content-Length if the server ignored Range).
    bool refreshedByRange = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HttpRequest request;
        request.method = QByteArrayLiteral("GET");
        request.url = QUrl(m_url);
        request.timeoutMs = 30000;
        request.headers.insert("User-Agent", m_userAgent.toUtf8());
        request.headers.insert("Accept-Encoding", "identity");
        request.headers.insert("Range", QByteArrayLiteral("bytes=0-0"));
        request.cancelled = cancelled;

        bool receivedData = false;
        const HttpResponse response = m_transport.sendStreaming(
            request,
            [&](const QByteArray &) {
                receivedData = true;
                return false; // Headers are enough; do not download the body.
            },
            nullptr);

        if (isCancelled(cancelled)) {
            throwCancelled();
        }
        if (response.isNetworkError() && !receivedData) {
            break;
        }

        if (isRefreshStatus(response.status) && !refreshedByRange && refreshUrl) {
            const QString next = refreshUrl();
            if (!next.isEmpty()) {
                m_url = next;
                refreshedByRange = true;
                continue;
            }
        }

        const ContentRange range = parseContentRange(response.header("Content-Range"));
        if (range.total >= 0) {
            m_size = range.total;
            m_sizeKnown = true;
            return m_size;
        }

        if (response.status == 200) {
            const qint64 length = contentLength(response);
            if (length >= 0) {
                m_size = length;
                m_sizeKnown = true;
                return m_size;
            }
        }
        break;
    }

    return m_size;
}

QByteArray RangeReader::read(qint64 offset, qint64 length,
                             const UrlRefresher &refreshUrl,
                             const Cancelled &cancelled)
{
    if (offset < 0 || length <= 0 || m_url.isEmpty()) {
        return {};
    }

    if (m_sizeKnown && offset >= m_size) {
        return {};
    }

    qint64 want = length;
    if (m_sizeKnown) {
        want = qMin(want, m_size - offset);
    }
    if (want <= 0) {
        return {};
    }

    const qint64 maxValue = std::numeric_limits<qint64>::max();
    const qint64 end = (want - 1 > maxValue - offset) ? maxValue : offset + want - 1;
    const QByteArray rangeHeader = QByteArrayLiteral("bytes=")
        + QByteArray::number(offset) + '-' + QByteArray::number(end);

    int networkRetries = 0;
    bool refreshed = false;

    for (;;) {
        if (isCancelled(cancelled)) {
            throwCancelled();
        }

        HttpRequest request;
        request.method = QByteArrayLiteral("GET");
        request.url = QUrl(m_url);
        request.timeoutMs = s_timeoutMs;
        request.headers.insert("User-Agent", m_userAgent.toUtf8());
        request.headers.insert("Accept-Encoding", "identity");
        request.headers.insert("Range", rangeHeader);
        request.cancelled = cancelled;

        QByteArray body;
        bool stoppedAfterRequestedLength = false;
        const HttpResponse response = m_transport.sendStreaming(
            request,
            [&](const QByteArray &chunk) {
                body.append(chunk);
                if (body.size() >= want) {
                    stoppedAfterRequestedLength = true;
                    return false;
                }
                return true;
            },
            nullptr);

        if (isCancelled(cancelled)) {
            throwCancelled();
        }

        // The transport reports an early stop as "cancelled".  That is a
        // success when we stopped because the requested range was complete.
        if (response.errorString == QLatin1String("cancelled")
            && !stoppedAfterRequestedLength) {
            throwCancelled();
        }

        if (response.isNetworkError() && !stoppedAfterRequestedLength) {
            retryOrThrow(networkRetries, cancelled);
            continue;
        }

        if (isRefreshStatus(response.status) && !refreshed && refreshUrl) {
            const QString next = refreshUrl();
            if (next.isEmpty()) {
                throw Error(KIO::ERR_WORKER_DEFINED,
                            QStringLiteral("Could not refresh the download URL"));
            }
            m_url = next;
            refreshed = true;
            continue;
        }

        if (isRetryableStatus(response.status) && !stoppedAfterRequestedLength) {
            retryOrThrow(networkRetries, cancelled);
            continue;
        }

        if (response.status == 416) {
            const ContentRange rejected = parseContentRange(response.header("Content-Range"));
            if (rejected.total >= 0) {
                m_size = rejected.total;
                m_sizeKnown = true;
            }
            if (m_sizeKnown && offset >= m_size) {
                return {};
            }
            throw Error(KIO::ERR_CANNOT_READ,
                        QStringLiteral("The server rejected the range request"));
        }

        if (response.status == 200 && offset > 0) {
            throw Error(KIO::ERR_CANNOT_SEEK,
                        QStringLiteral("The server ignored the range request"));
        }

        if (response.status != 200 && response.status != 206) {
            throw Error(kioCodeForHttp(response.status),
                        QStringLiteral("Chomikuj download failed (HTTP %1)")
                            .arg(response.status),
                        response.status);
        }

        const ContentRange range = parseContentRange(response.header("Content-Range"));
        if (range.start >= 0 && range.start != offset) {
            throw Error(KIO::ERR_CANNOT_SEEK,
                        QStringLiteral("The server returned a different range"));
        }
        if (range.total >= 0) {
            m_size = range.total;
            m_sizeKnown = true;
        } else if (response.status == 200) {
            const qint64 length = contentLength(response);
            if (length >= 0) {
                m_size = length;
                m_sizeKnown = true;
            }
        }

        if (body.isEmpty() && want > 0) {
            throw Error(KIO::ERR_CANNOT_READ,
                        QStringLiteral("The server returned an empty range"));
        }
        if (body.size() > want) {
            body.truncate(static_cast<int>(want));
        }
        return body;
    }
}

} // namespace Chomikuj

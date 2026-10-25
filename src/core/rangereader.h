/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "httptransport.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace Chomikuj
{

// A small stateful reader for one signed Chomikuj download URL.  It serves
// HTTP Range requests and keeps the exact file size learned from
// Content-Range/Content-Length.  It is independent of KIO so it can be tested
// without a worker or a FUSE mount.
class RangeReader
{
public:
    using UrlRefresher = std::function<QString()>;
    using Cancelled = std::function<bool()>;

    RangeReader(HttpTransport &transport, QString userAgent, QString url,
                qint64 fallbackSize, qint64 knownSize = -1);

    qint64 size() const;
    bool sizeKnown() const;
    QString url() const;

    // Probes the signed URL for the exact size with a one-byte Range request.
    // Updates the stored URL if the signed URL had to be refreshed.
    qint64 probeExactSize(const UrlRefresher &refreshUrl, const Cancelled &cancelled);

    // Reads at most `length` bytes.  Returns an empty array at EOF, and throws
    // when the server does not honour the range request or the connection
    // fails.  A short but non-empty result is valid; the caller may read again.
    QByteArray read(qint64 offset, qint64 length,
                    const UrlRefresher &refreshUrl, const Cancelled &cancelled);

private:
    HttpTransport &m_transport;
    QString m_userAgent;
    QString m_url;
    qint64 m_size = 0;
    bool m_sizeKnown = false;
};

} // namespace Chomikuj

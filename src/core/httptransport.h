/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <functional>

namespace Chomikuj
{

struct HttpRequest
{
    QByteArray method;
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    int timeoutMs = 30000;
    std::function<bool()> cancelled;
};

struct HttpResponse
{
    int status = 0;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    QString errorString;

    QByteArray header(const QByteArray &name) const;
    bool isNetworkError() const
    {
        return status == 0;
    }
};

using DataCallback = std::function<bool(const QByteArray &)>;
using ProgressCallback = std::function<void(qint64 current, qint64 total)>;

class HttpTransport
{
public:
    virtual ~HttpTransport();

    virtual HttpResponse send(const HttpRequest &request) = 0;
    virtual HttpResponse sendStreaming(const HttpRequest &request,
                                       const DataCallback &onData,
                                       const ProgressCallback &onProgress) = 0;
};

} // namespace Chomikuj

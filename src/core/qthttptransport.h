/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "httptransport.h"

#include <QNetworkAccessManager>

namespace Chomikuj
{

class QtHttpTransport : public HttpTransport
{
public:
    QtHttpTransport();

    HttpResponse send(const HttpRequest &request) override;
    HttpResponse sendStreaming(const HttpRequest &request,
                               const DataCallback &onData,
                               const ProgressCallback &onProgress) override;

private:
    QNetworkAccessManager m_manager;
};

} // namespace Chomikuj

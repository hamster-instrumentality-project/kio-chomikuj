/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "core/httptransport.h"

#include <functional>

class FakeTransport : public Chomikuj::HttpTransport
{
public:
    std::function<Chomikuj::HttpResponse(const Chomikuj::HttpRequest &)> handler;
    QList<Chomikuj::HttpRequest> requests;

    Chomikuj::HttpResponse send(const Chomikuj::HttpRequest &request) override
    {
        requests.append(request);
        if (handler) {
            return handler(request);
        }
        Chomikuj::HttpResponse response;
        response.status = 500;
        response.errorString = QStringLiteral("no fake handler");
        return response;
    }

    Chomikuj::HttpResponse sendStreaming(const Chomikuj::HttpRequest &request,
                                         const Chomikuj::DataCallback &onData,
                                         const Chomikuj::ProgressCallback &onProgress) override
    {
        requests.append(request);
        Chomikuj::HttpResponse response;
        if (handler) {
            response = handler(request);
        }
        if (!response.body.isEmpty() && onData && !onData(response.body)) {
            response.errorString = QStringLiteral("cancelled");
            return response;
        }
        if (onProgress) {
            onProgress(response.body.size(), response.body.size());
        }
        return response;
    }
};

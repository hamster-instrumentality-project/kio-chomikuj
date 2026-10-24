/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "httptransport.h"

namespace Chomikuj
{

QByteArray HttpResponse::header(const QByteArray &name) const
{
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return {};
}

HttpTransport::~HttpTransport() = default;

} // namespace Chomikuj

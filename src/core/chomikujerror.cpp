/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujerror.h"

#include <KIO/Global>

namespace Chomikuj
{

Error::Error(int kioCode, const QString &message, int httpStatus, int businessCode)
    : m_kioCode(kioCode)
    , m_httpStatus(httpStatus)
    , m_businessCode(businessCode)
    , m_message(message)
    , m_what(message.toUtf8())
{
}

int Error::kioCode() const
{
    return m_kioCode;
}

int Error::httpStatus() const
{
    return m_httpStatus;
}

int Error::businessCode() const
{
    return m_businessCode;
}

QString Error::message() const
{
    return m_message;
}

const char *Error::what() const noexcept
{
    return m_what.constData();
}

int kioCodeForHttp(int status)
{
    switch (status) {
    case 400:
        return KIO::ERR_WORKER_DEFINED;
    case 401:
        return KIO::ERR_CANNOT_LOGIN;
    case 403:
        return KIO::ERR_ACCESS_DENIED;
    case 404:
        return KIO::ERR_DOES_NOT_EXIST;
    case 423:
        return KIO::ERR_ACCESS_DENIED;
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
        return KIO::ERR_CONNECTION_BROKEN;
    case 505:
        return KIO::ERR_WORKER_DEFINED;
    default:
        return KIO::ERR_WORKER_DEFINED;
    }
}

int kioCodeForBusiness(int code)
{
    switch (code) {
    case 1: // SameName
    case 5: // NameExistsAtDestination
    case 2001:
    case 2003:
        return KIO::ERR_FILE_ALREADY_EXIST;
    case 2: // EmptyName
    case 4: // EndsWithDot
    case 8: // InvalidChars
    case 2000:
    case 2002:
    case 2008:
    case 2010:
    case 3002:
    case 3004:
        return KIO::ERR_WORKER_DEFINED;
    case 11:
    case 3404:
    case 601:
        return KIO::ERR_DOES_NOT_EXIST;
    case 12:
    case 602:
        return KIO::ERR_ACCESS_DENIED;
    case 605:
        return KIO::ERR_DISK_FULL;
    default:
        return KIO::ERR_WORKER_DEFINED;
    }
}

QString businessErrorMessage(int code, const QString &serverMessage)
{
    if (!serverMessage.isEmpty()) {
        return serverMessage;
    }
    switch (code) {
    case 1:
        return QStringLiteral("The name is unchanged");
    case 2:
        return QStringLiteral("The name is empty");
    case 4:
        return QStringLiteral("A name may not end with a dot");
    case 5:
        return QStringLiteral("A file or folder with this name already exists");
    case 8:
        return QStringLiteral("The name contains invalid characters");
    case 11:
        return QStringLiteral("The folder does not exist");
    case 12:
        return QStringLiteral("The password is incorrect");
    case 601:
        return QStringLiteral("The file does not exist");
    case 602:
        return QStringLiteral("Access denied");
    case 603:
        return QStringLiteral("The file is invalid");
    case 605:
        return QStringLiteral("Not enough transfer balance");
    default:
        return QStringLiteral("Chomikuj error %1").arg(code);
    }
}

} // namespace Chomikuj

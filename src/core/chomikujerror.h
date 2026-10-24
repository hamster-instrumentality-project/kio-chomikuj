/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>

#include <exception>

namespace Chomikuj
{

class Error : public std::exception
{
public:
    Error(int kioCode, const QString &message, int httpStatus = 0, int businessCode = 0);

    int kioCode() const;
    int httpStatus() const;
    int businessCode() const;
    QString message() const;

    const char *what() const noexcept override;

private:
    int m_kioCode;
    int m_httpStatus;
    int m_businessCode;
    QString m_message;
    QByteArray m_what;
};

// Mapping helpers shared by the client and the worker.
int kioCodeForHttp(int status);
int kioCodeForBusiness(int code);
QString businessErrorMessage(int code, const QString &serverMessage);

} // namespace Chomikuj

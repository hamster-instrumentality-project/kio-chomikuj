/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "signing.h"

#include <QCryptographicHash>
#include <QUrl>

namespace Chomikuj
{

namespace
{

// BuildConfig.SECRET_KEY, verbatim from the Android app.
constexpr const char s_buildConfigSecretKey[] =
    "4762,4883,4564,4765,3566,4687,3888,1449,1850,2731,3332,4053,2254,4615,"
    "4696,4297,1338,3859,1580,2021,4902,4704,4826,4471,4757,3502,4675,3848,"
    "1419,1758,2673,3298,4036,2183,4584,4660,4251,1231,3743,1527,1961,4874,"
    "4819,4898,4567,4776,3587,4689,3934,1536,1867,2723,3397,4073,2266,4603,"
    "4735,4352,1364,3879,1640,2025,4897,4775,4954,4596,4764,3611,4737,3933,"
    "1464,1894,2758,3333,4051,2260,4661,4711,4367,1351,3862,1638,2036,4943";

QString decodeSecret()
{
    const QStringList parts = QString::fromLatin1(s_buildConfigSecretKey).split(QLatin1Char(','));
    QList<int> values;
    values.reserve(parts.size());
    for (const QString &part : parts) {
        values.append(part.toInt());
    }

    const int quarter = static_cast<int>(values.size() * 0.25);
    if (quarter < 2) {
        return {};
    }

    QList<int> adjusted;
    adjusted.reserve(quarter);
    for (int i = 0; i < quarter; ++i) {
        adjusted.append(values.at(i) - (i + 2));
    }

    const int divisor = (adjusted.size() - 1) * 2;
    if (divisor == 0) {
        return {};
    }

    QString secret;
    secret.reserve(adjusted.size() - 1);
    for (int i = 0; i + 1 < adjusted.size(); ++i) {
        secret.append(QChar(adjusted.at(i) / divisor));
    }
    return secret;
}

} // namespace

QString decodedSecret()
{
    static const QString secret = decodeSecret();
    return secret;
}

QString sign(const QString &path, const QString &query, const QByteArray &body)
{
    QByteArray raw = path.toUtf8();
    if (!query.isEmpty()) {
        raw += '?';
        raw += query.toUtf8();
    }
    raw += body;
    raw += decodedSecret().toUtf8();

    // The application drops the leading slash before hashing.
    if (raw.startsWith('/')) {
        raw.remove(0, 1);
    }

    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Md5).toHex());
}

QString encodeQueryComponent(const QString &value)
{
    // Qt percent-encodes everything outside the unreserved set, including '/',
    // and uses '%20' for spaces, matching urllib.parse.quote(v, safe='').
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString buildQuery(const QList<QPair<QString, QString>> &parameters)
{
    QString query;
    bool first = true;
    for (const auto &parameter : parameters) {
        if (!first) {
            query += QLatin1Char('&');
        }
        first = false;
        query += encodeQueryComponent(parameter.first);
        query += QLatin1Char('=');
        query += encodeQueryComponent(parameter.second);
    }
    return query;
}

} // namespace Chomikuj

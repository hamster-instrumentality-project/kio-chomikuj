/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujclient.h"

#include "chomikujerror.h"
#include "signing.h"
#include "uploader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QThread>

#include <KIO/Global>

namespace Chomikuj
{

namespace
{

constexpr auto s_apiPrefix = "/api/v3";
constexpr auto s_clientVersion = "3.8.4";
constexpr int s_maxPage = 50;

const QList<int> s_successCodes = {0, 200, 201};

int jsonInt(const QJsonValue &value, int defaultValue = 0)
{
    if (value.isDouble()) {
        return value.toInt(defaultValue);
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : defaultValue;
    }
    return defaultValue;
}

qint64 jsonLongLong(const QJsonValue &value)
{
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        return value.toString().toLongLong();
    }
    return 0;
}

QString jsonString(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(static_cast<qint64>(value.toDouble()));
    }
    return {};
}

QString serverMessage(const QJsonObject &object)
{
    const QString message = object.value(QStringLiteral("Message")).toString();
    if (!message.isEmpty()) {
        return message;
    }
    return object.value(QStringLiteral("ErrorMessage")).toString();
}

QJsonArray idArray(const QList<qint64> &ids)
{
    QJsonArray array;
    for (qint64 id : ids) {
        array.append(static_cast<double>(id));
    }
    return array;
}

} // namespace

ChomikujClient::ChomikujClient(std::unique_ptr<HttpTransport> transport,
                               QString deviceId,
                               QString deviceName,
                               QString baseUrl)
    : m_transport(std::move(transport))
    , m_deviceId(std::move(deviceId))
    , m_deviceName(std::move(deviceName))
    , m_baseUrl(std::move(baseUrl))
{
    if (m_baseUrl.isEmpty()) {
        m_baseUrl = QString::fromLocal8Bit(qgetenv("CHOMIKUJ_BASE_URL"));
    }
    if (m_baseUrl.isEmpty()) {
        m_baseUrl = QStringLiteral("https://mobile.chomikuj.pl");
    }
    m_userAgent = QStringLiteral("android/%1 (%2; %3)")
                      .arg(QString::fromLatin1(s_clientVersion), m_deviceId, m_deviceName);
}

ChomikujClient::~ChomikujClient() = default;

QString ChomikujClient::userAgent() const
{
    return m_userAgent;
}

QString ChomikujClient::baseUrl() const
{
    return m_baseUrl;
}

HttpTransport &ChomikujClient::transport()
{
    return *m_transport;
}

bool ChomikujClient::hasApiKey() const
{
    return !m_apiKey.isEmpty();
}

QString ChomikujClient::apiKey() const
{
    return m_apiKey;
}

void ChomikujClient::clearApiKey()
{
    m_apiKey.clear();
}

QString ChomikujClient::accountId() const
{
    return m_accountId;
}

QString ChomikujClient::accountName() const
{
    return m_accountName;
}

QString ChomikujClient::avatarUrl() const
{
    return m_avatarUrl;
}

LoginResult ChomikujClient::login(const QString &accountName, const QString &password)
{
    QJsonObject body;
    body.insert(QStringLiteral("AccountName"), accountName);
    body.insert(QStringLiteral("Password"), password);

    const QJsonObject response = request(QByteArrayLiteral("POST"),
                                         QStringLiteral("/account/login"),
                                         {},
                                         &body,
                                         false,
                                         false);

    LoginResult result;
    result.accountId = jsonString(response.value(QStringLiteral("AccountId")));
    result.accountName = response.value(QStringLiteral("AccountName")).toString();
    result.apiKey = response.value(QStringLiteral("ApiKey")).toString();
    result.avatarUrl = response.value(QStringLiteral("AvatarUrl")).toString();

    if (result.apiKey.isEmpty()) {
        throw Error(KIO::ERR_CANNOT_LOGIN,
                    QStringLiteral("Chomikuj login returned no session key"));
    }

    m_apiKey = result.apiKey;
    m_accountId = result.accountId;
    m_accountName = result.accountName;
    m_avatarUrl = result.avatarUrl;
    return result;
}

QList<Entry> ChomikujClient::listFolder(qint64 parentId, qint64 accountId)
{
    QList<Entry> entries;
    for (int page = 1; page <= s_maxPage; ++page) {
        const Listing listing = listFolderPage(parentId, page, accountId);
        for (const Folder &folder : listing.folders) {
            if (folder.name.isEmpty()) {
                continue;
            }
            Entry entry;
            entry.id = folder.id;
            entry.name = folder.name;
            entry.isDir = true;
            entry.isHidden = folder.isHidden;
            entry.hasPassword = folder.hasPassword;
            entry.isAdult = folder.isAdult;
            entry.accountId = folder.accountId;
            entries.append(entry);
        }
        for (const File &file : listing.files) {
            if (file.fullName().isEmpty()) {
                continue;
            }
            Entry entry;
            entry.id = file.id;
            entry.name = file.fullName();
            entry.extension = file.extension;
            entry.sizeBytes = file.sizeBytes;
            entry.mediaType = file.mediaType;
            entry.isHidden = file.isHidden;
            entry.accountId = file.accountId;
            entries.append(entry);
        }
        if (!listing.nextPageAvailable) {
            break;
        }
    }
    return entries;
}

Listing ChomikujClient::listFolderPage(qint64 parentId, int page, qint64 accountId)
{
    const QJsonObject response = requestPage(parentId, page, accountId);

    Listing listing;
    listing.parentId = parentId;
    listing.parentName = response.value(QStringLiteral("ParentName")).toString();
    listing.nextPageAvailable = response.value(QStringLiteral("IsNextPageAvailable")).toBool();

    const QJsonArray folders = response.value(QStringLiteral("Folders")).toArray();
    for (const QJsonValue &value : folders) {
        const QJsonObject object = value.toObject();
        Folder folder;
        folder.id = jsonLongLong(object.value(QStringLiteral("Id")));
        folder.name = object.value(QStringLiteral("Name")).toString();
        folder.hasPassword = object.value(QStringLiteral("HasPassword")).toBool();
        folder.isAdult = object.value(QStringLiteral("IsAdult")).toBool();
        folder.isHidden = object.value(QStringLiteral("IsHidden")).toBool();
        folder.isPublicUploadEnabled = object.value(QStringLiteral("IsPublicUploadEnabled")).toBool();
        folder.accountId = jsonLongLong(object.value(QStringLiteral("AccountId")));
        listing.folders.append(folder);
    }

    const QJsonArray files = response.value(QStringLiteral("Files")).toArray();
    for (const QJsonValue &value : files) {
        const QJsonObject object = value.toObject();
        File file;
        file.id = jsonLongLong(object.value(QStringLiteral("FileId")));
        file.name = object.value(QStringLiteral("FileName")).toString();
        file.extension = object.value(QStringLiteral("FileType")).toString();
        if (file.extension.startsWith(QLatin1Char('.'))) {
            file.extension.remove(0, 1);
        }
        file.sizeBytes = jsonLongLong(object.value(QStringLiteral("Size"))) * 1024;
        file.mediaType = object.value(QStringLiteral("MediaType")).toString();
        file.isHidden = object.value(QStringLiteral("IsHidden")).toBool();
        file.accountId = jsonLongLong(object.value(QStringLiteral("AccountId")));
        listing.files.append(file);
    }

    return listing;
}

QJsonObject ChomikujClient::requestPage(qint64 parentId, int page, qint64 accountId)
{
    QList<QPair<QString, QString>> query = {
        {QStringLiteral("Parent"), QString::number(parentId)},
        {QStringLiteral("page"), QString::number(page)},
    };
    if (accountId > 0) {
        query.append({QStringLiteral("AccountId"), QString::number(accountId)});
    }
    return request(QByteArrayLiteral("GET"), QStringLiteral("/folders"), query);
}

QList<AccountSearchItem> ChomikujClient::searchAccounts(const QString &query)
{
    QList<AccountSearchItem> results;
    for (int page = 1; page <= s_maxPage; ++page) {
        const QJsonObject response = request(
            QByteArrayLiteral("GET"),
            QStringLiteral("/account/search"),
            {{QStringLiteral("Query"), query},
             {QStringLiteral("PageNumber"), QString::number(page)}});

        const QJsonArray items = response.value(QStringLiteral("Results")).toArray();
        for (const QJsonValue &value : items) {
            const QJsonObject object = value.toObject();
            AccountSearchItem item;
            item.id = jsonLongLong(object.value(QStringLiteral("AccountId")));
            item.name = object.value(QStringLiteral("AccountName")).toString();
            if (item.id > 0 && !item.name.isEmpty()) {
                results.append(item);
            }
        }
        if (!response.value(QStringLiteral("IsNextPageAvailable")).toBool()) {
            break;
        }
    }
    return results;
}

void ChomikujClient::folderPassword(qint64 accountId, qint64 folderId, const QString &password)
{
    QJsonObject body;
    body.insert(QStringLiteral("AccountId"), static_cast<double>(accountId));
    body.insert(QStringLiteral("FolderId"), static_cast<double>(folderId));
    body.insert(QStringLiteral("Password"), password);
    request(QByteArrayLiteral("POST"), QStringLiteral("/folders/password"), {}, &body);
}

qint64 ChomikujClient::createFolder(const QString &name, qint64 parentId)
{
    QJsonObject body;
    body.insert(QStringLiteral("FolderName"), name);
    body.insert(QStringLiteral("ParentId"), static_cast<double>(parentId));

    const QJsonObject response = request(QByteArrayLiteral("POST"),
                                         QStringLiteral("/folders/create"),
                                         {},
                                         &body);
    return jsonLongLong(response.value(QStringLiteral("FolderId")));
}

void ChomikujClient::changeFileName(qint64 fileId, const QString &newName)
{
    QJsonObject body;
    body.insert(QStringLiteral("FileId"), static_cast<double>(fileId));
    body.insert(QStringLiteral("FileName"), newName);
    request(QByteArrayLiteral("POST"), QStringLiteral("/files/changeName"), {}, &body);
}

void ChomikujClient::changeFolderName(qint64 folderId, const QString &newName)
{
    QJsonObject body;
    body.insert(QStringLiteral("FolderId"), static_cast<double>(folderId));
    body.insert(QStringLiteral("FolderName"), newName);
    request(QByteArrayLiteral("POST"), QStringLiteral("/folders/changeName"), {}, &body);
}

void ChomikujClient::deleteItems(const QList<qint64> &fileIds, const QList<qint64> &folderIds)
{
    QJsonObject body;
    body.insert(QStringLiteral("Files"), idArray(fileIds));
    body.insert(QStringLiteral("Folders"), idArray(folderIds));
    request(QByteArrayLiteral("POST"), QStringLiteral("/files/delete"), {}, &body);
}

void ChomikujClient::moveItems(qint64 sourceFolderId, qint64 targetFolderId,
                               const QList<qint64> &fileIds, const QList<qint64> &folderIds)
{
    QJsonObject body;
    body.insert(QStringLiteral("TargetFolderId"), static_cast<double>(targetFolderId));
    body.insert(QStringLiteral("SourceFolderId"), static_cast<double>(sourceFolderId));
    body.insert(QStringLiteral("Files"), idArray(fileIds));
    body.insert(QStringLiteral("Folders"), idArray(folderIds));
    request(QByteArrayLiteral("POST"), QStringLiteral("/files/move"), {}, &body);
}

QString ChomikujClient::downloadUrl(qint64 fileId)
{
    const QJsonObject response = request(QByteArrayLiteral("GET"),
                                         QStringLiteral("/files/download"),
                                         {{QStringLiteral("FileId"), QString::number(fileId)}});
    const QString url = response.value(QStringLiteral("FileUrl")).toString();
    if (url.isEmpty()) {
        throw Error(KIO::ERR_WORKER_DEFINED,
                    QStringLiteral("Chomikuj returned no download URL"));
    }
    return url;
}

void ChomikujClient::downloadFile(qint64 fileId,
                                  const DataCallback &onData,
                                  const ProgressCallback &onProgress,
                                  const std::function<bool()> &cancelled)
{
    QString url = downloadUrl(fileId);
    qint64 offset = 0;
    bool refreshed = false;
    const int maxRefresh = 1;

    while (true) {
        HttpRequest request;
        request.method = QByteArrayLiteral("GET");
        request.url = QUrl(url);
        request.headers.insert("User-Agent", m_userAgent.toUtf8());
        request.timeoutMs = 120000;
        request.cancelled = cancelled;
        if (offset > 0) {
            request.headers.insert("Range", QByteArrayLiteral("bytes=") + QByteArray::number(offset) + '-');
        }

        if (onProgress) {
            onProgress(offset, -1);
        }

        const HttpResponse response = m_transport->sendStreaming(
            request,
            onData,
            [&](qint64 current, qint64 total) {
                if (onProgress) {
                    onProgress(offset + current, total >= 0 ? offset + total : -1);
                }
            });

        if (response.errorString == QLatin1String("cancelled")
            || (cancelled && cancelled())) {
            throw Error(KIO::ERR_USER_CANCELED, QStringLiteral("Download cancelled"));
        }

        if (response.isNetworkError()) {
            throw Error(KIO::ERR_CONNECTION_BROKEN,
                        response.errorString.isEmpty()
                            ? QStringLiteral("Chomikuj download failed")
                            : response.errorString);
        }

        if ((response.status == 401 || response.status == 403 || response.status == 404 || response.status == 410)
            && !refreshed && maxRefresh > 0) {
            refreshed = true;
            url = downloadUrl(fileId);
            continue;
        }

        if (response.status != 200 && response.status != 206) {
            throw Error(kioCodeForHttp(response.status),
                        QStringLiteral("Chomikuj download failed (HTTP %1)").arg(response.status),
                        response.status);
        }

        if (offset > 0 && response.status == 200) {
            throw Error(KIO::ERR_CANNOT_RESUME,
                        QStringLiteral("The server ignored the resume request"));
        }

        return;
    }
}

UploadSlot ChomikujClient::negotiateUpload(const QString &name, qint64 size,
                                           qint64 folderId, const QString &crc32Hex)
{
    QJsonObject body;
    body.insert(QStringLiteral("Name"), name);
    body.insert(QStringLiteral("Size"), static_cast<double>(size));
    body.insert(QStringLiteral("FolderId"), static_cast<double>(folderId));
    body.insert(QStringLiteral("Hash"), crc32Hex);

    const QJsonObject response = request(QByteArrayLiteral("POST"),
                                         QStringLiteral("/files/upload/partialUpload"),
                                         {},
                                         &body,
                                         true,
                                         true);

    UploadSlot slot;
    slot.url = response.value(QStringLiteral("Url")).toString();
    slot.chunk = jsonLongLong(response.value(QStringLiteral("Chunk")));
    slot.completed = response.value(QStringLiteral("UploadCompleted")).toBool();
    return slot;
}

void ChomikujClient::uploadFile(const QString &localPath, qint64 folderId,
                                const QString &remoteName,
                                const ProgressCallback &progress,
                                const std::function<bool()> &cancelled)
{
    Chomikuj::uploadFile(*this, localPath, folderId, remoteName, progress, cancelled);
}

QJsonObject ChomikujClient::request(const QByteArray &method,
                                    const QString &path,
                                    const QList<QPair<QString, QString>> &query,
                                    const QJsonObject *body,
                                    bool authenticated,
                                    bool mayRetry)
{
    const QString queryString = buildQuery(query);
    const QByteArray bodyBytes = body ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray();
    const QString fullPath = QString::fromLatin1(s_apiPrefix) + path;
    const QString token = sign(fullPath, queryString, bodyBytes);

    QString url = m_baseUrl + fullPath;
    if (!queryString.isEmpty()) {
        url += QLatin1Char('?') + queryString;
    }

    const bool idempotent = method == QByteArrayLiteral("GET")
        || method == QByteArrayLiteral("HEAD")
        || method == QByteArrayLiteral("DELETE")
        || method == QByteArrayLiteral("OPTIONS");

    for (int attempt = 0;; ++attempt) {
        HttpRequest httpRequest;
        httpRequest.method = method;
        httpRequest.url = QUrl(url);
        httpRequest.body = bodyBytes;
        httpRequest.timeoutMs = 30000;
        httpRequest.headers.insert("User-Agent", m_userAgent.toUtf8());
        httpRequest.headers.insert("Accept", "application/json");
        httpRequest.headers.insert("Token", token.toUtf8());
        if (authenticated && !m_apiKey.isEmpty()) {
            httpRequest.headers.insert("Api-Key", m_apiKey.toUtf8());
        }
        if (body) {
            httpRequest.headers.insert("Content-Type", "application/json; charset=utf-8");
        }

        const HttpResponse response = m_transport->send(httpRequest);
        const bool retryableStatus = response.status == 408
            || response.status == 429
            || response.status == 500
            || response.status == 502
            || response.status == 503
            || response.status == 504;

        if ((response.isNetworkError() || retryableStatus)
            && attempt < m_maxRetries
            && (idempotent || mayRetry || response.status == 429)) {
            QThread::msleep(static_cast<unsigned long>(500 * (1 << attempt)));
            continue;
        }

        if (response.isNetworkError()) {
            throw Error(KIO::ERR_CONNECTION_BROKEN,
                        response.errorString.isEmpty()
                            ? QStringLiteral("Chomikuj request failed")
                            : response.errorString);
        }

        QJsonObject object;
        if (!response.body.isEmpty()) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(response.body, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                if (response.status >= 400) {
                    throw Error(kioCodeForHttp(response.status),
                                QStringLiteral("Chomikuj API error (HTTP %1)").arg(response.status),
                                response.status);
                }
                throw Error(KIO::ERR_WORKER_DEFINED,
                            QStringLiteral("Invalid JSON from Chomikuj: %1").arg(parseError.errorString()));
            }
            object = document.object();
        }

        const int businessCode = jsonInt(object.value(QStringLiteral("Code")), -1);
        if (response.status >= 400) {
            const QString message = serverMessage(object);
            throw Error(kioCodeForHttp(response.status),
                        message.isEmpty()
                            ? QStringLiteral("Chomikuj API error (HTTP %1)").arg(response.status)
                            : message,
                        response.status,
                        businessCode);
        }

        if (businessCode >= 0 && !s_successCodes.contains(businessCode)) {
            const QString message = businessErrorMessage(businessCode, serverMessage(object));
            throw Error(kioCodeForBusiness(businessCode), message, response.status, businessCode);
        }

        return object;
    }
}

} // namespace Chomikuj

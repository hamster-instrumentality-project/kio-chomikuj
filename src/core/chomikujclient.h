/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "chomikujtypes.h"
#include "httptransport.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

#include <functional>
#include <memory>

namespace Chomikuj
{

class ChomikujClient
{
public:
    ChomikujClient(std::unique_ptr<HttpTransport> transport,
                   QString deviceId,
                   QString deviceName,
                   QString baseUrl = QString());
    ~ChomikujClient();

    ChomikujClient(const ChomikujClient &) = delete;
    ChomikujClient &operator=(const ChomikujClient &) = delete;

    QString userAgent() const;
    QString baseUrl() const;
    HttpTransport &transport();

    bool hasApiKey() const;
    QString apiKey() const;
    void clearApiKey();

    QString accountId() const;
    QString accountName() const;
    QString avatarUrl() const;

    LoginResult login(const QString &accountName, const QString &password);

    QList<Entry> listFolder(qint64 parentId);
    Listing listFolderPage(qint64 parentId, int page);

    qint64 createFolder(const QString &name, qint64 parentId);
    void changeFileName(qint64 fileId, const QString &newName);
    void changeFolderName(qint64 folderId, const QString &newName);
    void deleteItems(const QList<qint64> &fileIds, const QList<qint64> &folderIds);
    void moveItems(qint64 sourceFolderId, qint64 targetFolderId,
                   const QList<qint64> &fileIds, const QList<qint64> &folderIds);

    QString downloadUrl(qint64 fileId);
    void downloadFile(qint64 fileId,
                      const DataCallback &onData,
                      const ProgressCallback &onProgress,
                      const std::function<bool()> &cancelled);

    UploadSlot negotiateUpload(const QString &name, qint64 size,
                               qint64 folderId, const QString &crc32Hex);
    void uploadFile(const QString &localPath, qint64 folderId,
                    const QString &remoteName,
                    const ProgressCallback &progress,
                    const std::function<bool()> &cancelled);

    QJsonObject request(const QByteArray &method,
                        const QString &path,
                        const QList<QPair<QString, QString>> &query = {},
                        const QJsonObject *body = nullptr,
                        bool authenticated = true,
                        bool mayRetry = false);

private:
    QJsonObject requestPage(qint64 parentId, int page);

    std::unique_ptr<HttpTransport> m_transport;
    QString m_deviceId;
    QString m_deviceName;
    QString m_baseUrl;
    QString m_userAgent;
    QString m_apiKey;
    QString m_accountId;
    QString m_accountName;
    QString m_avatarUrl;
    int m_maxRetries = 2;
};

} // namespace Chomikuj

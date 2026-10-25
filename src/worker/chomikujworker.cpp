/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "core/chomikujfilesystem.h"
#include "core/chomikujtypes.h"
#include "core/qthttptransport.h"
#include "core/rangereader.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QUuid>

#include <KConfigGroup>
#include <KIO/AuthInfo>
#include <KIO/Global>
#include <KIO/WorkerBase>
#include <KLocalizedString>
#include <KSharedConfig>

#include <sys/stat.h>

#include <cstdio>
#include <limits>
#include <memory>

using Chomikuj::AccountSearchItem;
using Chomikuj::ChomikujClient;
using Chomikuj::ChomikujFileSystem;
using Chomikuj::Entry;
using Chomikuj::Error;
using Chomikuj::RangeReader;

namespace {
QString configString(const char *key, const QString &defaultValue = {})
{
    const KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                             QStringLiteral("General"));
    return group.readEntry(QString::fromLatin1(key), defaultValue);
}

QString streamingConfigString(const char *key, const QString &defaultValue)
{
    QString suffix;
    const QString camel = QString::fromLatin1(key);
    for (const QChar character : camel) {
        if (character.isUpper() && !suffix.isEmpty()) {
            suffix += QLatin1Char('_');
        }
        suffix += character.toUpper();
    }
    const QString environmentKey = QStringLiteral("CHOMIKUJ_STREAMING_") + suffix;
    const QString environmentValue = qEnvironmentVariable(environmentKey.toUtf8().constData());
    if (!environmentValue.isEmpty()) {
        return environmentValue;
    }

    // KSharedConfig does not notice edits made by another process (the KCM)
    // unless it is told to reparse.  Workers are long-lived, so check the file
    // mtime before reading streaming settings.
    static qint64 lastConfigMtime = -1;
    const QString configPath =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/chomikujrc");
    const qint64 configMtime = QFileInfo(configPath).lastModified().toMSecsSinceEpoch();
    if (configMtime != lastConfigMtime) {
        KSharedConfig::openConfig(QStringLiteral("chomikujrc"))->reparseConfiguration();
        lastConfigMtime = configMtime;
    }

    const KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                             QStringLiteral("Streaming"));
    return group.readEntry(QString::fromLatin1(key), defaultValue);
}

QString streamingTargetUrlPolicy()
{
    const QString value = streamingConfigString("TargetUrl", QStringLiteral("read-only"));
    return value.trimmed().toLower();
}

QUrl childUrl(const QUrl &directoryUrl, const QString &name)
{
    QUrl result = directoryUrl;
    QString path = result.path();
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    path += name;
    result.setPath(path);
    result.setQuery(QString());
    result.setFragment(QString());
    return result;
}

bool isVideoEntry(const Entry &entry, const QString &mimeType)
{
    if (entry.isDir) {
        return false;
    }
    if (entry.mediaType.compare(QLatin1String("Video"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return mimeType.startsWith(QLatin1String("video/"));
}

void writeConfigString(const char *key, const QString &value)
{
    KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                       QStringLiteral("General"));
    group.writeEntry(QString::fromLatin1(key), value);
    group.sync();
}

QString deviceId()
{
    QString id = configString("DeviceId");
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeConfigString("DeviceId", id);
    }
    return id;
}

QString deviceName()
{
    return configString("DeviceName", QStringLiteral("KDE"));
}

QString pathOf(const QUrl &url)
{
    const QString path = url.path();
    return path.isEmpty() ? QStringLiteral("/") : path;
}

} // namespace

class ChomikujWorker : public KIO::WorkerBase
{
public:
    ChomikujWorker(const QByteArray &protocol, const QByteArray &poolSocket,
                   const QByteArray &appSocket);
    ~ChomikujWorker() override;

    void setHost(const QString &host, quint16 port, const QString &user,
                 const QString &pass) override;

    KIO::WorkerResult openConnection() override;
    void closeConnection() override;
    KIO::WorkerResult listDir(const QUrl &url) override;
    KIO::WorkerResult stat(const QUrl &url) override;
    KIO::WorkerResult mimetype(const QUrl &url) override;
    KIO::WorkerResult get(const QUrl &url) override;
    KIO::WorkerResult open(const QUrl &url, QIODevice::OpenMode mode) override;
    KIO::WorkerResult read(KIO::filesize_t size) override;
    KIO::WorkerResult write(const QByteArray &data) override;
    KIO::WorkerResult seek(KIO::filesize_t offset) override;
    KIO::WorkerResult truncate(KIO::filesize_t size) override;
    KIO::WorkerResult close() override;
    KIO::WorkerResult put(const QUrl &url, int permissions, KIO::JobFlags flags) override;
    KIO::WorkerResult mkdir(const QUrl &url, int permissions) override;
    KIO::WorkerResult rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags) override;
    KIO::WorkerResult copy(const QUrl &src, const QUrl &dest, int permissions,
                           KIO::JobFlags flags) override;
    KIO::WorkerResult del(const QUrl &url, bool isfile) override;
    KIO::WorkerResult fileSystemFreeSpace(const QUrl &url) override;

private:
    void ensureAuthenticated(bool forceReLogin = false, const QString &errorMessage = {},
                             bool allowPrompt = true);
    void configureFileSystem();
    bool promptFolderPassword(qint64 folderId, QString *password, const QString &errorMessage);
    ChomikujClient &client();
    QString downloadUrlFor(qint64 fileId, bool forceRefresh = false);
    bool shouldUseStreamingTarget(const Entry &entry, const QString &mimeType) const;
    bool isStreamingProtocol() const;
    void resetOpenState();
    void clearReadAhead();

    KIO::UDSEntry udsEntry(const Entry &entry, const QUrl &url) const;
    KIO::UDSEntry dotEntry() const;

    template<typename Function>
    auto withAuthRetry(Function &&function, bool allowPrompt = true) -> decltype(function())
    {
        const bool effectivePrompt = allowPrompt
            && metaData(QStringLiteral("no-auth-prompt")) != QLatin1String("true");
        ensureAuthenticated(false, {}, effectivePrompt);
        try {
            return function();
        } catch (const Error &error) {
            if (error.businessCode() == 12) {
                throw; // Protected folder; handled by the filesystem password provider.
            }
            if (error.httpStatus() == 401) {
                m_client->clearApiKey();
                ensureAuthenticated(true, {}, effectivePrompt);
                return function();
            }
            throw;
        }
    }

    static KIO::WorkerResult failure(const Error &error)
    {
        return KIO::WorkerResult::fail(error.kioCode(), error.message());
    }

    static KIO::WorkerResult failure(const std::exception &error)
    {
        return KIO::WorkerResult::fail(KIO::ERR_WORKER_DEFINED,
                                       QString::fromUtf8(error.what()));
    }

    QString m_protocolName;
    QString m_targetAccount;
    QString m_loginAccount;
    bool m_targetExplicit = false;
    bool m_nonInteractive = false;
    std::unique_ptr<ChomikujClient> m_client;
    std::unique_ptr<ChomikujFileSystem> m_fileSystem;

    struct CachedUrl {
        QString url;
        qint64 fetchedAt = 0;
    };
    QHash<qint64, CachedUrl> m_urlCache;
    QHash<qint64, qint64> m_sizeCache;

    std::unique_ptr<RangeReader> m_rangeReader;
    qint64 m_openFileId = 0;
    qint64 m_openOffset = 0;
    qint64 m_openSize = 0;
    bool m_openSizeKnown = false;
    bool m_open = false;

    // kio-fuse opens a new FileJob for every FUSE read.  Keep a small
    // read-ahead buffer across those opens so sequential playback does not
    // issue one HTTP Range request per 128-512 KiB read.
    qint64 m_readAheadFileId = 0;
    qint64 m_readAheadOffset = 0;
    QByteArray m_readAhead;
};

ChomikujWorker::ChomikujWorker(const QByteArray &protocol, const QByteArray &poolSocket,
                               const QByteArray &appSocket)
    : KIO::WorkerBase(protocol, poolSocket, appSocket)
    , m_protocolName(QString::fromLatin1(protocol))
{
}

ChomikujWorker::~ChomikujWorker() = default;

void ChomikujWorker::setHost(const QString &host, quint16 port, const QString &user,
                             const QString &pass)
{
    Q_UNUSED(port)
    Q_UNUSED(pass)
    const QString requestedAccount = user.isEmpty() ? host : user;
    m_targetExplicit = !requestedAccount.isEmpty();
    m_targetAccount = requestedAccount;
    if (m_targetAccount.isEmpty()) {
        m_targetAccount = configString("LastAccount");
    }
    m_loginAccount.clear();
    m_fileSystem.reset();
    resetOpenState();
    clearReadAhead();
    m_urlCache.clear();
    m_sizeCache.clear();
    if (m_client) {
        m_client->clearApiKey();
    }
}

bool ChomikujWorker::isStreamingProtocol() const
{
    return m_protocolName == QLatin1String("chomikujstream");
}

void ChomikujWorker::resetOpenState()
{
    m_rangeReader.reset();
    m_openFileId = 0;
    m_openOffset = 0;
    m_openSize = 0;
    m_openSizeKnown = false;
    m_open = false;
}

void ChomikujWorker::clearReadAhead()
{
    m_readAheadFileId = 0;
    m_readAheadOffset = 0;
    m_readAhead.clear();
}

ChomikujClient &ChomikujWorker::client()
{
    if (!m_client) {
        m_client = std::make_unique<ChomikujClient>(
            std::make_unique<Chomikuj::QtHttpTransport>(),
            deviceId(), deviceName());
    }
    return *m_client;
}

QString ChomikujWorker::downloadUrlFor(qint64 fileId, bool forceRefresh)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh) {
        const auto cached = m_urlCache.constFind(fileId);
        if (cached != m_urlCache.cend() && !cached->url.isEmpty()
            && now - cached->fetchedAt < 300000) {
            return cached->url;
        }
    }

    const QString url = withAuthRetry(
        [&] {
            return client().downloadUrl(fileId);
        },
        false);
    m_urlCache.insert(fileId, CachedUrl{url, now});
    return url;
}

bool ChomikujWorker::shouldUseStreamingTarget(const Entry &entry,
                                              const QString &mimeType) const
{
    if (isStreamingProtocol() || !isVideoEntry(entry, mimeType)) {
        return false;
    }

    const QString policy = streamingTargetUrlPolicy();
    if (policy == QLatin1String("all")) {
        return true;
    }
    if (policy == QLatin1String("read-only")) {
        return m_fileSystem && m_fileSystem->isReadOnly();
    }
    return false;
}

void ChomikujWorker::ensureAuthenticated(bool forceReLogin, const QString &errorMessage,
                                         bool allowPrompt)
{
    if (!forceReLogin && m_client && m_client->hasApiKey()) {
        if (!m_fileSystem) {
            configureFileSystem();
        }
        return;
    }

    if (!m_client) {
        m_client = std::make_unique<ChomikujClient>(
            std::make_unique<Chomikuj::QtHttpTransport>(),
            deviceId(), deviceName());
    }
    if (forceReLogin) {
        m_client->clearApiKey();
    }

    if (m_loginAccount.isEmpty()) {
        m_loginAccount = configString("LastAccount");
    }
    const QString environmentAccount = qEnvironmentVariable("CHOMIKUJ_ACCOUNT");
    if (!environmentAccount.isEmpty()) {
        m_loginAccount = environmentAccount;
    }
    if (m_loginAccount.isEmpty()) {
        m_loginAccount = m_targetAccount;
    }

    KIO::AuthInfo authInfo;
    authInfo.caption = i18n("Chomikuj.pl");
    authInfo.keepPassword = true;
    authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_loginAccount));
    authInfo.username = m_loginAccount;
    authInfo.realmValue = QStringLiteral("chomikuj");

    const QString environmentPassword = qEnvironmentVariable("CHOMIKUJ_PASSWORD");
    if (!environmentPassword.isEmpty()) {
        authInfo.password = environmentPassword;
    }

    if (m_loginAccount.isEmpty()) {
        if (!allowPrompt) {
            throw Error(KIO::ERR_CANNOT_LOGIN,
                        i18n("Authentication is required. Open the Chomikuj account once to sign in."));
        }
        authInfo.prompt = i18n("Enter your Chomikuj.pl account name and password");
        const int dialogResult = openPasswordDialog(authInfo, errorMessage);
        if (dialogResult != 0 || authInfo.username.isEmpty()) {
            throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
        }
        m_loginAccount = authInfo.username;
        authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_loginAccount));
    } else if (authInfo.password.isEmpty()) {
        authInfo.prompt = i18n("Enter the password for Chomikuj account %1", m_loginAccount);
        if (!checkCachedAuthentication(authInfo) && allowPrompt) {
            const int dialogResult = openPasswordDialog(authInfo, errorMessage);
            if (dialogResult != 0) {
                throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
            }
            if (!authInfo.username.isEmpty()) {
                m_loginAccount = authInfo.username;
                authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_loginAccount));
            }
        }
    }

    if (authInfo.password.isEmpty()) {
        if (!allowPrompt) {
            throw Error(KIO::ERR_CANNOT_LOGIN,
                        i18n("Authentication is required. Open the Chomikuj account once to sign in."));
        }
        authInfo.prompt = i18n("Enter the password for Chomikuj account %1", m_loginAccount);
        const int dialogResult = openPasswordDialog(authInfo, errorMessage);
        if (dialogResult != 0 || authInfo.password.isEmpty()) {
            throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
        }
        if (!authInfo.username.isEmpty()) {
            m_loginAccount = authInfo.username;
            authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_loginAccount));
        }
    }

    try {
        client().login(m_loginAccount, authInfo.password);
    } catch (const Error &error) {
        if (allowPrompt && error.httpStatus() == 401 && errorMessage.isEmpty()) {
            authInfo.password.clear();
            const int dialogResult = openPasswordDialog(
                authInfo, i18n("The password for “%1” was rejected.", m_loginAccount));
            if (dialogResult != 0) {
                throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
            }
            if (!authInfo.username.isEmpty()) {
                m_loginAccount = authInfo.username;
                authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_loginAccount));
            }
            client().login(m_loginAccount, authInfo.password);
        } else {
            throw;
        }
    }

    if (allowPrompt && authInfo.keepPassword && environmentPassword.isEmpty()) {
        cacheAuthentication(authInfo);
    }
    writeConfigString("LastAccount", m_loginAccount);

    if (m_targetAccount.isEmpty() || !m_targetExplicit) {
        m_targetAccount = m_loginAccount;
    }
    configureFileSystem();
}

void ChomikujWorker::configureFileSystem()
{
    m_fileSystem.reset();
    if (!m_client) {
        return;
    }

    if (m_targetAccount.isEmpty()) {
        m_targetAccount = m_client->accountName();
    }
    if (m_loginAccount.isEmpty()) {
        m_loginAccount = m_client->accountName();
    }

    qint64 listAccountId = 0;
    qint64 ownerAccountId = m_client->accountId().toLongLong();
    bool readOnly = isStreamingProtocol();

    const bool isOwnAccount =
        m_targetAccount.compare(m_client->accountName(), Qt::CaseInsensitive) == 0
        || m_targetAccount.compare(m_loginAccount, Qt::CaseInsensitive) == 0;

    if (!isOwnAccount) {
        const QList<AccountSearchItem> results = m_client->searchAccounts(m_targetAccount);
        qint64 targetId = 0;
        for (const AccountSearchItem &item : results) {
            if (item.name.compare(m_targetAccount, Qt::CaseInsensitive) == 0) {
                targetId = item.id;
                break;
            }
        }
        if (targetId <= 0) {
            throw Error(KIO::ERR_DOES_NOT_EXIST,
                        i18n("No Chomikuj account named “%1” was found", m_targetAccount));
        }
        listAccountId = targetId;
        ownerAccountId = targetId;
        readOnly = true;
    }

    m_fileSystem = std::make_unique<ChomikujFileSystem>(client(), listAccountId,
                                                        ownerAccountId, readOnly);
    m_fileSystem->setPasswordProvider(
        [this](qint64 folderId, QString *password, const QString &error) {
            return promptFolderPassword(folderId, password, error);
        });
}

bool ChomikujWorker::promptFolderPassword(qint64 folderId, QString *password,
                                          const QString &errorMessage)
{
    KIO::AuthInfo authInfo;
    authInfo.caption = i18n("Chomikuj.pl");
    authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_targetAccount));
    authInfo.username = m_loginAccount;
    authInfo.realmValue = QStringLiteral("chomikuj-folder-%1").arg(folderId);
    authInfo.prompt = i18n("Enter the password for the protected folder");
    authInfo.keepPassword = true;

    if (checkCachedAuthentication(authInfo) && !authInfo.password.isEmpty()) {
        *password = authInfo.password;
        return true;
    }

    if (m_nonInteractive) {
        return false;
    }

    const int dialogResult = openPasswordDialog(authInfo, errorMessage);
    if (dialogResult != 0 || authInfo.password.isEmpty()) {
        return false;
    }
    if (authInfo.keepPassword) {
        cacheAuthentication(authInfo);
    }
    *password = authInfo.password;
    return true;
}

KIO::WorkerResult ChomikujWorker::openConnection()
{
    try {
        ensureAuthenticated();
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

void ChomikujWorker::closeConnection()
{
    resetOpenState();
    clearReadAhead();
    m_nonInteractive = false;
}

KIO::UDSEntry ChomikujWorker::udsEntry(const Entry &entry, const QUrl &url) const
{
    KIO::UDSEntry uds;
    QString name = entry.name;
    if (entry.id == 0 && entry.isDir && name == QLatin1String("/")) {
        name = url.fileName();
        if (name.isEmpty()) {
            name = QStringLiteral(".");
        }
    }

    uds.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    uds.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, name);
    uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, entry.isDir ? S_IFDIR : S_IFREG);
    uds.fastInsert(KIO::UDSEntry::UDS_SIZE, entry.isDir ? 0 : entry.sizeBytes);

    const bool readOnly = m_fileSystem && m_fileSystem->isReadOnly();
    if (entry.isDir) {
        qlonglong access = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
        if (!readOnly) {
            access |= S_IWUSR | S_IWGRP;
        }
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS, access);
        QString icon = QStringLiteral("folder");
        if (entry.hasPassword) {
            icon = QStringLiteral("folder-locked");
        } else if (entry.isAdult) {
            icon = QStringLiteral("folder-red");
        }
        uds.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, icon);
        if (entry.isHidden) {
            uds.fastInsert(KIO::UDSEntry::UDS_HIDDEN, 1);
        }
    } else {
        qlonglong access = S_IRUSR | S_IRGRP | S_IROTH;
        if (!readOnly) {
            access |= S_IWUSR;
        }
        const QString mimeType = Chomikuj::mimeTypeForName(entry.name);
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS, access);
        uds.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mimeType);

        if (shouldUseStreamingTarget(entry, mimeType)) {
            QUrl target = url;
            target.setScheme(QStringLiteral("chomikujstream"));
            target.setQuery(QString());
            target.setFragment(QString());
            uds.fastInsert(KIO::UDSEntry::UDS_TARGET_URL,
                           target.toString(QUrl::FullyEncoded));
        }
    }

    const QString account = m_targetAccount.isEmpty()
        ? (m_client ? m_client->accountName() : QString())
        : m_targetAccount;
    if (!account.isEmpty()) {
        uds.fastInsert(KIO::UDSEntry::UDS_USER, account);
    }
    return uds;
}

KIO::UDSEntry ChomikujWorker::dotEntry() const
{
    KIO::UDSEntry uds;
    uds.fastInsert(KIO::UDSEntry::UDS_NAME, QStringLiteral("."));
    uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    uds.fastInsert(KIO::UDSEntry::UDS_SIZE, 0);

    qlonglong access = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    if (!(m_fileSystem && m_fileSystem->isReadOnly())) {
        access |= S_IWUSR | S_IWGRP;
    }
    uds.fastInsert(KIO::UDSEntry::UDS_ACCESS, access);
    uds.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, QStringLiteral("folder"));
    return uds;
}

KIO::WorkerResult ChomikujWorker::listDir(const QUrl &url)
{
    try {
        const QList<Entry> entries = withAuthRetry([&] {
            return m_fileSystem->list(pathOf(url));
        });
        for (const Entry &entry : entries) {
            listEntry(udsEntry(entry, childUrl(url, entry.name)));
        }
        listEntry(dotEntry());
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::stat(const QUrl &url)
{
    try {
        const Entry entry = withAuthRetry([&] {
            return m_fileSystem->stat(pathOf(url));
        });
        statEntry(udsEntry(entry, url));
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::mimetype(const QUrl &url)
{
    try {
        const Entry entry = withAuthRetry([&] {
            return m_fileSystem->stat(pathOf(url));
        });
        mimeType(entry.isDir ? QStringLiteral("inode/directory")
                             : Chomikuj::mimeTypeForName(entry.name));
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::get(const QUrl &url)
{
    try {
        const Entry entry = withAuthRetry([&] {
            return m_fileSystem->stat(pathOf(url));
        });
        if (entry.isDir) {
            return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY, url.path());
        }

        mimeType(Chomikuj::mimeTypeForName(entry.name));
        totalSize(entry.sizeBytes);

        QString rangeStart = metaData(QStringLiteral("range-start"));
        if (rangeStart.isEmpty()) {
            rangeStart = metaData(QStringLiteral("resume"));
        }
        bool offsetOk = false;
        const qint64 startOffset = rangeStart.isEmpty()
            ? 0
            : rangeStart.toLongLong(&offsetOk);
        const qint64 resumeOffset = (offsetOk && startOffset > 0) ? startOffset : 0;
        if (!rangeStart.isEmpty()) {
            canResume();
        }

        qint64 processed = resumeOffset;
        withAuthRetry([&] {
            m_fileSystem->download(
                pathOf(url),
                [&](const QByteArray &chunk) {
                    if (wasKilled()) {
                        return false;
                    }
                    data(chunk);
                    processed += chunk.size();
                    processedSize(processed);
                    return true;
                },
                [&](qint64 current, qint64 total) {
                    if (total >= 0) {
                        totalSize(total);
                    }
                    if (current >= 0) {
                        processedSize(current);
                    }
                },
                [&] {
                    return wasKilled();
                },
                resumeOffset);
        });
        data(QByteArray());
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::open(const QUrl &url, QIODevice::OpenMode mode)
{
    if ((mode & QIODevice::ReadOnly) == 0
        || (mode & (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Truncate))) {
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_OPEN_FOR_WRITING, url.path());
    }

    resetOpenState();
    m_nonInteractive = true;
    try {
        const Entry entry = withAuthRetry(
            [&] {
                return m_fileSystem->stat(pathOf(url));
            },
            false);
        if (entry.isDir) {
            m_nonInteractive = false;
            return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY, url.path());
        }
        if (entry.id != m_readAheadFileId) {
            clearReadAhead();
        }

        const QString urlForFile = downloadUrlFor(entry.id);
        const qint64 fallbackSize = qMax<qint64>(0, entry.sizeBytes);
        const qint64 knownSize = m_sizeCache.value(entry.id, -1);
        auto reader = std::make_unique<RangeReader>(
            client().transport(), client().userAgent(), urlForFile,
            fallbackSize, knownSize);

        if (knownSize < 0) {
            reader->probeExactSize(
                [&] {
                    return downloadUrlFor(entry.id, true);
                },
                [&] {
                    return wasKilled();
                });
            if (reader->sizeKnown()) {
                m_sizeCache.insert(entry.id, reader->size());
            }
        }

        if (!reader->sizeKnown() && reader->size() == 0) {
            m_nonInteractive = false;
            resetOpenState();
            return KIO::WorkerResult::fail(
                KIO::ERR_CANNOT_READ,
                i18n("Could not determine the exact size of “%1”", entry.name));
        }

        m_urlCache.insert(entry.id,
                          CachedUrl{reader->url(), QDateTime::currentMSecsSinceEpoch()});
        m_openFileId = entry.id;
        m_openOffset = 0;
        m_openSize = reader->size();
        m_openSizeKnown = reader->sizeKnown();
        m_rangeReader = std::move(reader);
        m_open = true;
        m_nonInteractive = false;

        mimeType(Chomikuj::mimeTypeForName(entry.name));
        totalSize(m_openSize);
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        m_nonInteractive = false;
        resetOpenState();
        return failure(error);
    } catch (const std::exception &error) {
        m_nonInteractive = false;
        resetOpenState();
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::read(KIO::filesize_t size)
{
    if (!m_open || !m_rangeReader) {
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_READ, QString());
    }

    constexpr qint64 maxReadBytes = 8 * 1024 * 1024;
    if (size == 0 || (m_openSizeKnown && m_openOffset >= m_openSize)) {
        data(QByteArray());
        return KIO::WorkerResult::pass();
    }

    const qint64 requested = qMin<qint64>(static_cast<qint64>(size), maxReadBytes);
    const qint64 want = m_openSizeKnown
        ? qMin<qint64>(requested, m_openSize - m_openOffset)
        : requested;
    if (want <= 0) {
        data(QByteArray());
        return KIO::WorkerResult::pass();
    }

    try {
        constexpr qint64 readAheadBytes = 1024 * 1024;

        // Serve from a previous FileJob's read-ahead buffer when possible.
        if (m_readAheadFileId == m_openFileId
            && m_openOffset >= m_readAheadOffset
            && m_openOffset < m_readAheadOffset + m_readAhead.size()) {
            const qint64 available =
                m_readAheadOffset + m_readAhead.size() - m_openOffset;
            const qint64 take = qMin(want, available);
            const QByteArray chunk = m_readAhead.mid(
                static_cast<int>(m_openOffset - m_readAheadOffset),
                static_cast<int>(take));
            data(chunk);
            m_openOffset += chunk.size();
            return KIO::WorkerResult::pass();
        }

        qint64 fetch = qMax(want, readAheadBytes);
        if (m_openSizeKnown) {
            fetch = qMin(fetch, m_openSize - m_openOffset);
        }

        const QByteArray body = m_rangeReader->read(
            m_openOffset,
            fetch,
            [&] {
                return downloadUrlFor(m_openFileId, true);
            },
            [&] {
                return wasKilled();
            });

        if (m_rangeReader->sizeKnown()) {
            const bool wasKnown = m_openSizeKnown;
            m_openSizeKnown = true;
            if (!wasKnown || m_rangeReader->size() != m_openSize) {
                m_openSize = m_rangeReader->size();
                m_sizeCache.insert(m_openFileId, m_openSize);
                totalSize(m_openSize);
            }
        }

        if (body.isEmpty()) {
            if (m_openSizeKnown && m_openOffset >= m_openSize) {
                data(QByteArray());
                return KIO::WorkerResult::pass();
            }
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_READ,
                                           QStringLiteral("The server returned no data"));
        }

        m_urlCache.insert(m_openFileId,
                          CachedUrl{m_rangeReader->url(),
                                    QDateTime::currentMSecsSinceEpoch()});
        m_readAheadFileId = m_openFileId;
        m_readAheadOffset = m_openOffset;
        m_readAhead = body;
        const qint64 take = qMin(want, body.size());
        data(body.left(static_cast<int>(take)));
        m_openOffset += take;
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::write(const QByteArray &data)
{
    Q_UNUSED(data)
    return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, QString());
}

KIO::WorkerResult ChomikujWorker::seek(KIO::filesize_t offset)
{
    if (!m_open) {
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_SEEK, QString());
    }
    if (offset > static_cast<KIO::filesize_t>(std::numeric_limits<qint64>::max())) {
        return KIO::WorkerResult::fail(KIO::ERR_CANNOT_SEEK, QString());
    }

    m_openOffset = static_cast<qint64>(offset);
    position(offset);
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult ChomikujWorker::truncate(KIO::filesize_t size)
{
    Q_UNUSED(size)
    return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, QString());
}

KIO::WorkerResult ChomikujWorker::close()
{
    resetOpenState();
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult ChomikujWorker::put(const QUrl &url, int permissions, KIO::JobFlags flags)
{
    Q_UNUSED(permissions)
    try {
        ensureAuthenticated();
        if (m_fileSystem->isReadOnly()) {
            return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED, url.path());
        }
        // We always upload from offset 0, but FileCopyJob expects this signal
        // before it starts pumping data from the source job.
        canResume(0);

        const QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString tempDir = tempRoot.isEmpty() ? QDir::tempPath() : tempRoot;
        QTemporaryFile tempFile(tempDir + QStringLiteral("/chomikuj-upload-XXXXXX"));
        tempFile.setAutoRemove(true);
        if (!tempFile.open()) {
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_WRITE, tempFile.fileName());
        }

        qint64 received = 0;
        bool sourceSizeKnown = false;
        const qint64 sourceSize = metaData(QStringLiteral("sourceSize")).toLongLong(&sourceSizeKnown);
        totalSize(sourceSizeKnown ? sourceSize : 0);
        for (;;) {
            QByteArray buffer;
            dataReq();
            const int read = readData(buffer);
            if (read < 0) {
                return KIO::WorkerResult::fail(KIO::ERR_CANNOT_READ, url.path());
            }
            if (read == 0) {
                break;
            }
            if (tempFile.write(buffer) != buffer.size()) {
                return KIO::WorkerResult::fail(KIO::ERR_CANNOT_WRITE, tempFile.fileName());
            }
            received += buffer.size();
            if (sourceSizeKnown) {
                processedSize(qMin(received, sourceSize) / 2);
            }
            if (wasKilled()) {
                return KIO::WorkerResult::fail(KIO::ERR_USER_CANCELED, url.path());
            }
        }

        const qint64 size = tempFile.size();
        tempFile.flush();
        tempFile.close();
        totalSize(size);
        processedSize(size / 2);

        const bool overwrite = flags & KIO::Overwrite;
        withAuthRetry([&] {
            m_fileSystem->upload(
                tempFile.fileName(),
                pathOf(url),
                overwrite,
                [&](qint64 current, qint64) {
                    processedSize(qMin(size, size / 2 + current / 2));
                },
                [&] {
                    return wasKilled();
                });
        });
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::mkdir(const QUrl &url, int permissions)
{
    Q_UNUSED(permissions)
    try {
        withAuthRetry([&] {
            m_fileSystem->mkdir(pathOf(url));
        });
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags)
{
    try {
        withAuthRetry([&] {
            m_fileSystem->rename(pathOf(src), pathOf(dest), flags & KIO::Overwrite);
        });
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::copy(const QUrl &src, const QUrl &dest, int permissions,
                                       KIO::JobFlags flags)
{
    Q_UNUSED(permissions)
    try {
        ensureAuthenticated();
        if (m_fileSystem->isReadOnly()) {
            return KIO::WorkerResult::fail(KIO::ERR_ACCESS_DENIED, dest.path());
        }

        if (src.isLocalFile()) {
            const QFileInfo localFile(src.toLocalFile());
            if (!localFile.isFile()) {
                return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, src.path());
            }
            totalSize(localFile.size());
            withAuthRetry([&] {
                m_fileSystem->upload(
                    localFile.absoluteFilePath(),
                    pathOf(dest),
                    flags & KIO::Overwrite,
                    [&](qint64 current, qint64 total) {
                        if (total >= 0) {
                            totalSize(total);
                        }
                        processedSize(current);
                    },
                    [&] {
                        return wasKilled();
                    });
            });
            return KIO::WorkerResult::pass();
        }

        if (pathOf(src) == pathOf(dest)) {
            return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, dest.path());
        }
        const Entry source = withAuthRetry([&] {
            return m_fileSystem->stat(pathOf(src));
        });
        if (source.isDir) {
            // Directory copies are handled recursively by the KIO copy job.
            return KIO::WorkerResult::fail(KIO::ERR_UNSUPPORTED_ACTION, src.path());
        }

        const QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString tempDir = tempRoot.isEmpty() ? QDir::tempPath() : tempRoot;
        QTemporaryFile tempFile(tempDir + QStringLiteral("/chomikuj-copy-XXXXXX"));
        tempFile.setAutoRemove(true);
        if (!tempFile.open()) {
            return KIO::WorkerResult::fail(KIO::ERR_CANNOT_WRITE, tempFile.fileName());
        }

        // The API reports sizes rounded down to whole kilobytes; use the
        // listing size only as the first approximation.  Reserve the first
        // half of the progress range for the download and the second half for
        // the upload so the bar never jumps back to zero.
        const qint64 listedSize = qMax<qint64>(1, source.sizeBytes);
        totalSize(listedSize);
        qint64 downloaded = 0;
        withAuthRetry([&] {
            m_fileSystem->download(
                pathOf(src),
                [&](const QByteArray &chunk) {
                    if (wasKilled()) {
                        return false;
                    }
                    if (tempFile.write(chunk) != chunk.size()) {
                        return false;
                    }
                    downloaded += chunk.size();
                    processedSize(qMin(downloaded, listedSize) / 2);
                    return true;
                },
                nullptr,
                [&] {
                    return wasKilled();
                });
        });
        tempFile.flush();
        tempFile.close();

        const qint64 copySize = tempFile.size();
        totalSize(copySize);
        processedSize(copySize / 2);
        withAuthRetry([&] {
            m_fileSystem->upload(
                tempFile.fileName(),
                pathOf(dest),
                flags & KIO::Overwrite,
                [&](qint64 current, qint64) {
                    processedSize(qMin(copySize, copySize / 2 + current / 2));
                },
                [&] {
                    return wasKilled();
                });
        });
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::del(const QUrl &url, bool isfile)
{
    try {
        const bool recursive = metaData(QStringLiteral("recurse")) == QLatin1String("true");
        withAuthRetry([&] {
            m_fileSystem->remove(pathOf(url), isfile, recursive);
        });
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::fileSystemFreeSpace(const QUrl &url)
{
    Q_UNUSED(url)
    // Chomikuj exposes no storage capacity and the transfer balance is not a
    // byte count.  Leaving total/available unset avoids bogus "disk full"
    // prompts in file managers.
    return KIO::WorkerResult::pass();
}

class KIOPluginForMetaData : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.kio.slave.chomikuj" FILE "chomikuj.json")
};

extern "C" {
int Q_DECL_EXPORT kdemain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kio_chomikuj"));
    KLocalizedString::setApplicationDomain("kio6_chomikuj");

    if (argc != 4) {
        fprintf(stderr, "Usage: kio_chomikuj protocol domain-socket1 domain-socket2\n");
        return 1;
    }

    ChomikujWorker worker(argv[1], argv[2], argv[3]);
    worker.dispatchLoop();
    return 0;
}
}

#include "chomikujworker.moc"

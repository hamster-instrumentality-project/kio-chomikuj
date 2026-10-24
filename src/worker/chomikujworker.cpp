/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "core/chomikujfilesystem.h"
#include "core/chomikujtypes.h"
#include "core/qthttptransport.h"

#include <QCoreApplication>
#include <QDir>
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

#include <memory>

using Chomikuj::ChomikujClient;
using Chomikuj::ChomikujFileSystem;
using Chomikuj::Entry;
using Chomikuj::Error;

namespace {
QString configString(const char *key, const QString &defaultValue = {})
{
    const KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                             QStringLiteral("General"));
    return group.readEntry(QString::fromLatin1(key), defaultValue);
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
    KIO::WorkerResult listDir(const QUrl &url) override;
    KIO::WorkerResult stat(const QUrl &url) override;
    KIO::WorkerResult mimetype(const QUrl &url) override;
    KIO::WorkerResult get(const QUrl &url) override;
    KIO::WorkerResult put(const QUrl &url, int permissions, KIO::JobFlags flags) override;
    KIO::WorkerResult mkdir(const QUrl &url, int permissions) override;
    KIO::WorkerResult rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags) override;
    KIO::WorkerResult copy(const QUrl &src, const QUrl &dest, int permissions,
                           KIO::JobFlags flags) override;
    KIO::WorkerResult del(const QUrl &url, bool isfile) override;
    KIO::WorkerResult fileSystemFreeSpace(const QUrl &url) override;

private:
    void ensureAuthenticated(bool forceReLogin = false, const QString &errorMessage = {});
    ChomikujClient &client();

    KIO::UDSEntry udsEntry(const Entry &entry, const QUrl &url) const;
    KIO::UDSEntry dotEntry() const;

    template<typename Function>
    auto withAuthRetry(Function &&function) -> decltype(function())
    {
        ensureAuthenticated();
        try {
            return function();
        } catch (const Error &error) {
            if (error.httpStatus() == 401) {
                m_client->clearApiKey();
                ensureAuthenticated(true);
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

    QString m_account;
    std::unique_ptr<ChomikujClient> m_client;
    std::unique_ptr<ChomikujFileSystem> m_fileSystem;
};

ChomikujWorker::ChomikujWorker(const QByteArray &protocol, const QByteArray &poolSocket,
                               const QByteArray &appSocket)
    : KIO::WorkerBase(protocol, poolSocket, appSocket)
{
}

ChomikujWorker::~ChomikujWorker() = default;

void ChomikujWorker::setHost(const QString &host, quint16 port, const QString &user,
                             const QString &pass)
{
    Q_UNUSED(port)
    Q_UNUSED(pass)
    m_account = user.isEmpty() ? host : user;
    if (m_account.isEmpty()) {
        m_account = configString("LastAccount");
    }
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

void ChomikujWorker::ensureAuthenticated(bool forceReLogin, const QString &errorMessage)
{
    if (!forceReLogin && m_client && m_client->hasApiKey()) {
        return;
    }

    if (!m_client) {
        m_client = std::make_unique<ChomikujClient>(
            std::make_unique<Chomikuj::QtHttpTransport>(),
            deviceId(), deviceName());
    }

    if (m_account.isEmpty()) {
        m_account = configString("LastAccount");
    }
    const QString environmentAccount = qEnvironmentVariable("CHOMIKUJ_ACCOUNT");
    if (m_account.isEmpty() && !environmentAccount.isEmpty()) {
        m_account = environmentAccount;
    }

    KIO::AuthInfo authInfo;
    authInfo.caption = i18n("Chomikuj.pl");
    authInfo.keepPassword = true;
    authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_account));
    authInfo.username = m_account;
    authInfo.realmValue = QStringLiteral("chomikuj");

    const QString environmentPassword = qEnvironmentVariable("CHOMIKUJ_PASSWORD");
    if (!environmentPassword.isEmpty()) {
        authInfo.password = environmentPassword;
    }

    if (m_account.isEmpty()) {
        authInfo.prompt = i18n("Enter your Chomikuj.pl account name and password");
        const int dialogResult = openPasswordDialog(authInfo, errorMessage);
        if (dialogResult != 0 || authInfo.username.isEmpty()) {
            throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
        }
        m_account = authInfo.username;
        authInfo.url = QUrl(QStringLiteral("chomikuj://%1/").arg(m_account));
    } else if (authInfo.password.isEmpty()) {
        authInfo.prompt = i18n("Enter the password for Chomikuj account %1", m_account);
        if (!checkCachedAuthentication(authInfo)) {
            const int dialogResult = openPasswordDialog(authInfo, errorMessage);
            if (dialogResult != 0) {
                throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
            }
        }
    }

    if (authInfo.password.isEmpty()) {
        authInfo.prompt = i18n("Enter the password for Chomikuj account %1", m_account);
        const int dialogResult = openPasswordDialog(authInfo, errorMessage);
        if (dialogResult != 0 || authInfo.password.isEmpty()) {
            throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
        }
    }

    try {
        client().login(m_account, authInfo.password);
    } catch (const Error &error) {
        if (error.httpStatus() == 401 && errorMessage.isEmpty()) {
            authInfo.password.clear();
            const int dialogResult = openPasswordDialog(
                authInfo, i18n("The password for “%1” was rejected.", m_account));
            if (dialogResult != 0) {
                throw Error(KIO::ERR_CANNOT_LOGIN, i18n("Authentication cancelled"));
            }
            client().login(m_account, authInfo.password);
        } else {
            throw;
        }
    }

    if (authInfo.keepPassword && environmentPassword.isEmpty()) {
        cacheAuthentication(authInfo);
    }
    writeConfigString("LastAccount", m_account);
    m_fileSystem = std::make_unique<ChomikujFileSystem>(client());
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

    if (entry.isDir) {
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                       static_cast<qlonglong>(S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH));
        uds.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, QStringLiteral("folder"));
        if (entry.isHidden) {
            uds.fastInsert(KIO::UDSEntry::UDS_HIDDEN, 1);
        }
    } else {
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                       static_cast<qlonglong>(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
        uds.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE,
                       Chomikuj::mimeTypeForName(entry.name));
    }

    const QString account = m_client ? m_client->accountName() : QString();
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
    uds.fastInsert(KIO::UDSEntry::UDS_ACCESS,
                   static_cast<qlonglong>(S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH));
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
            listEntry(udsEntry(entry, url));
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

        qint64 processed = 0;
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
                });
        });
        data(QByteArray());
        return KIO::WorkerResult::pass();
    } catch (const Error &error) {
        return failure(error);
    } catch (const std::exception &error) {
        return failure(error);
    }
}

KIO::WorkerResult ChomikujWorker::put(const QUrl &url, int permissions, KIO::JobFlags flags)
{
    Q_UNUSED(permissions)
    try {
        ensureAuthenticated();
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
        totalSize(0);
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
            processedSize(received);
            if (wasKilled()) {
                return KIO::WorkerResult::fail(KIO::ERR_USER_CANCELED, url.path());
            }
        }

        const qint64 size = tempFile.size();
        tempFile.flush();
        tempFile.close();
        totalSize(size);

        const bool overwrite = flags & KIO::Overwrite;
        withAuthRetry([&] {
            m_fileSystem->upload(
                tempFile.fileName(),
                pathOf(url),
                overwrite,
                [&](qint64 current, qint64 total) {
                    if (total >= 0 && total != size) {
                        totalSize(total);
                    }
                    processedSize(current);
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

        totalSize(source.sizeBytes);
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
                    processedSize(downloaded);
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
                });
        });
        tempFile.flush();
        tempFile.close();

        totalSize(tempFile.size());
        processedSize(0);
        withAuthRetry([&] {
            m_fileSystem->upload(
                tempFile.fileName(),
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

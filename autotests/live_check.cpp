/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Gated live verification against the real Chomikuj Mobile API.
 *
 * Read-only by default.  With --write it creates a unique temporary folder,
 * uploads a small file, lists it, renames it, moves it to a second folder,
 * downloads and compares it, and finally deletes both folders (which cascades
 * server-side).  It never touches existing user data.
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "core/qthttptransport.h"
#include "core/rangereader.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>

#include <cstdio>
#include <functional>
#include <memory>

using namespace Chomikuj;

namespace
{

int fail(const QString &message)
{
    QTextStream(stderr) << "chomikuj live check: " << message << '\n';
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("chomikuj-live-check"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Live, self-cleaning check of the Chomikuj protocol client"));
    parser.addHelpOption();
    QCommandLineOption writeOption(QStringLiteral("write"),
                                   QStringLiteral("Run the create/upload/rename/move/download/delete round-trip"));
    QCommandLineOption keepOption(QStringLiteral("keep"),
                                  QStringLiteral("Keep temporary folders for debugging"));
    QCommandLineOption publicOption(QStringLiteral("public"),
                                    QStringLiteral("List the public root of the given account (read-only)"),
                                    QStringLiteral("account"));
    QCommandLineOption rangeOption(
        QStringLiteral("range"),
        QStringLiteral("Also verify exact size and HTTP Range reads (requires --write)"));
    parser.addOption(writeOption);
    parser.addOption(keepOption);
    parser.addOption(publicOption);
    parser.addOption(rangeOption);
    parser.process(app);

    const QString account = qEnvironmentVariable("CHOMIKUJ_ACCOUNT");
    const QString password = qEnvironmentVariable("CHOMIKUJ_PASSWORD");
    if (account.isEmpty() || password.isEmpty()) {
        QTextStream(stdout) << "chomikuj live check: CHOMIKUJ_ACCOUNT/CHOMIKUJ_PASSWORD not set; skipping\n";
        return 0;
    }

    auto client = std::make_unique<ChomikujClient>(
        std::make_unique<QtHttpTransport>(),
        QStringLiteral("kio-chomikuj-live"),
        QStringLiteral("KIO"));

    QList<qint64> foldersToDelete;
    auto cleanup = [&] {
        if (parser.isSet(keepOption)) {
            return;
        }
        for (qint64 folder : foldersToDelete) {
            try {
                client->deleteItems({}, {folder});
            } catch (const Error &) {
            }
        }
    };
    struct ScopeExit {
        std::function<void()> function;
        ~ScopeExit()
        {
            function();
        }
    } cleanupGuard{cleanup};

    try {
        const LoginResult login = client->login(account, password);
        QTextStream(stdout) << "logged in as " << login.accountName
                            << " (id " << login.accountId << ")\n";

        const QList<Entry> root = client->listFolder(0);
        QTextStream(stdout) << "root lists " << root.size() << " entries\n";

        if (parser.isSet(publicOption)) {
            const QString wanted = parser.value(publicOption);
            const QList<AccountSearchItem> matches = client->searchAccounts(wanted);
            qint64 accountId = 0;
            for (const AccountSearchItem &item : matches) {
                if (item.name.compare(wanted, Qt::CaseInsensitive) == 0) {
                    accountId = item.id;
                    break;
                }
            }
            if (accountId <= 0) {
                return fail(QStringLiteral("no public account named %1").arg(wanted));
            }
            const QList<Entry> publicRoot = client->listFolder(0, accountId);
            QTextStream(stdout) << "public account " << wanted << " (id " << accountId
                                << ") lists " << publicRoot.size() << " entries\n";
        }

        if (!parser.isSet(writeOption)) {
            if (parser.isSet(rangeOption)) {
                return fail(QStringLiteral("--range requires --write"));
            }
            return 0;
        }

        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        const QString folderAName = QStringLiteral("kio-chomikuj-live-a-%1").arg(suffix);
        const QString folderBName = QStringLiteral("kio-chomikuj-live-b-%1").arg(suffix);
        const qint64 folderA = client->createFolder(folderAName, 0);
        foldersToDelete.append(folderA);
        const qint64 folderB = client->createFolder(folderBName, 0);
        foldersToDelete.append(folderB);
        QTextStream(stdout) << "created temporary folders " << folderA << " and " << folderB << '\n';

        QTemporaryDir tempDir;
        if (!tempDir.isValid()) {
            return fail(QStringLiteral("could not create a temporary directory"));
        }
        const QString localPath = tempDir.filePath(QStringLiteral("probe.txt"));
        const QByteArray content = "kio-chomikuj live probe\n";
        {
            QFile file(localPath);
            if (!file.open(QIODevice::WriteOnly) || file.write(content) != qint64(content.size())) {
                return fail(QStringLiteral("could not write the local probe file"));
            }
        }

        const QString uploadedName = QStringLiteral("probe-%1.txt").arg(suffix);
        client->uploadFile(localPath, folderA, uploadedName, nullptr, [] { return false; });

        QList<Entry> listing = client->listFolder(folderA);
        qint64 fileId = 0;
        Entry uploadedEntry;
        for (const Entry &entry : listing) {
            if (entry.name == uploadedName) {
                fileId = entry.id;
                uploadedEntry = entry;
            }
        }
        if (fileId == 0) {
            return fail(QStringLiteral("uploaded file did not appear in the folder listing"));
        }

        if (parser.isSet(rangeOption)) {
            RangeReader reader(client->transport(), client->userAgent(),
                               client->downloadUrl(fileId),
                               uploadedEntry.sizeBytes);
            const qint64 exactSize = reader.probeExactSize(
                [&] {
                    return client->downloadUrl(fileId);
                },
                [] {
                    return false;
                });
            if (exactSize != content.size()) {
                return fail(QStringLiteral("exact size probe returned %1, expected %2")
                                .arg(exactSize)
                                .arg(content.size()));
            }

            const QList<qint64> offsets{0, 1, content.size() - 1};
            for (qint64 offset : offsets) {
                const qint64 wanted = qMin<qint64>(5, exactSize - offset);
                const QByteArray part = reader.read(
                    offset, wanted,
                    [&] {
                        return client->downloadUrl(fileId);
                    },
                    [] {
                        return false;
                    });
                if (part != content.mid(offset, wanted)) {
                    return fail(QStringLiteral("range read at offset %1 differed").arg(offset));
                }
            }

            const QByteArray pastEnd = reader.read(
                exactSize, 5,
                [&] {
                    return client->downloadUrl(fileId);
                },
                [] {
                    return false;
                });
            if (!pastEnd.isEmpty()) {
                return fail(QStringLiteral("read past EOF returned data"));
            }
            QTextStream(stdout) << "range reads verified (" << exactSize << " bytes)\n";
        }

        const QString renamed = QStringLiteral("renamed-%1.txt").arg(suffix);
        client->changeFileName(fileId, renamed);
        listing = client->listFolder(folderA);
        bool sawRenamed = false;
        for (const Entry &entry : listing) {
            if (entry.id == fileId) {
                sawRenamed = entry.name == renamed;
            }
        }
        if (!sawRenamed) {
            return fail(QStringLiteral("rename produced an unexpected filename: %1")
                            .arg(listing.isEmpty() ? QStringLiteral("<empty>") : listing.first().name));
        }

        client->moveItems(folderA, folderB, {fileId}, {});
        listing = client->listFolder(folderB);
        if (listing.isEmpty() || listing.first().id != fileId) {
            return fail(QStringLiteral("moved file did not appear in the target folder"));
        }

        QByteArray downloaded;
        client->downloadFile(
            fileId,
            [&](const QByteArray &chunk) {
                downloaded += chunk;
                return true;
            },
            nullptr,
            [] { return false; });
        if (downloaded != content) {
            return fail(QStringLiteral("downloaded bytes differ from the uploaded file"));
        }

        QTextStream(stdout) << "round-trip succeeded\n";
        return 0;
    } catch (const Error &error) {
        return fail(error.message());
    } catch (const std::exception &error) {
        return fail(QString::fromUtf8(error.what()));
    }
}

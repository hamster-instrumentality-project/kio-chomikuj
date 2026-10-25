/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "core/chomikujfilesystem.h"
#include "faketransport.h"

#include <KIO/Global>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtTest>

using Chomikuj::ChomikujClient;
using Chomikuj::ChomikujFileSystem;
using Chomikuj::HttpResponse;

namespace
{

HttpResponse jsonResponse(int status, const QByteArray &body)
{
    HttpResponse response;
    response.status = status;
    response.headers.insert("Content-Type", "application/json");
    response.body = body;
    return response;
}

QByteArray listing(const QByteArray &folders, const QByteArray &files)
{
    return "{\"Code\":0,\"Folders\":[" + folders + "],\"Files\":[" + files
        + "],\"IsNextPageAvailable\":false}";
}

struct Fixture
{
    FakeTransport *fake = nullptr;
    std::unique_ptr<ChomikujClient> client;
    std::unique_ptr<ChomikujFileSystem> fs;

    Fixture()
    {
        auto transport = std::make_unique<FakeTransport>();
        fake = transport.get();
        client = std::make_unique<ChomikujClient>(std::move(transport),
                                                  QStringLiteral("dev-id"),
                                                  QStringLiteral("TestDevice"),
                                                  QStringLiteral("https://example.test"));

        fake->handler = [this](const Chomikuj::HttpRequest &request) {
            return handle(request);
        };
        client->login(QStringLiteral("user"), QStringLiteral("pass"));
        fs = std::make_unique<ChomikujFileSystem>(*client);
    }

    HttpResponse handle(const Chomikuj::HttpRequest &request)
    {
        const QString path = request.url.path();
        if (path == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        if (path == QLatin1String("/api/v3/folders")) {
            const QString parent = QUrlQuery(request.url).queryItemValue(QStringLiteral("Parent"));
            if (parent == QLatin1String("0")) {
                return jsonResponse(200,
                                    listing("{\"Id\":7,\"Name\":\"Music\",\"HasPassword\":false}",
                                            "{\"FileId\":1,\"FileName\":\"root\","
                                            "\"FileType\":\"txt\",\"Size\":2}"));
            }
            if (parent == QLatin1String("7")) {
                return jsonResponse(200,
                                    listing("{\"Id\":8,\"Name\":\"Sub\"}",
                                            "{\"FileId\":2,\"FileName\":\"A\","
                                            "\"FileType\":\"mp3\",\"Size\":1},"
                                            "{\"FileId\":3,\"FileName\":\"existing\","
                                            "\"FileType\":\"bin\",\"Size\":1}"));
            }
            if (parent == QLatin1String("8")) {
                return jsonResponse(200, listing("", ""));
            }
            return jsonResponse(404, "{\"Code\":11,\"Message\":\"No folder\"}");
        }
        if (path == QLatin1String("/api/v3/folders/create")) {
            return jsonResponse(200, "{\"Code\":0,\"FolderId\":9}");
        }
        if (path == QLatin1String("/api/v3/files/changeName")
            || path == QLatin1String("/api/v3/folders/changeName")) {
            return jsonResponse(200, "{\"Code\":0}");
        }
        if (path == QLatin1String("/api/v3/files/move")
            || path == QLatin1String("/api/v3/files/delete")) {
            return jsonResponse(200, "{}");
        }
        if (path == QLatin1String("/api/v3/files/upload/partialUpload")) {
            return jsonResponse(200, "{\"Code\":0,\"Url\":\"\",\"Chunk\":0,"
                                       "\"UploadCompleted\":true}");
        }
        return jsonResponse(500, "{}");
    }
};

} // namespace

class FileSystemTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void listAndStat();
    void statIsCached();
    void missing();
    void mkdir();
    void rename();
    void moveAcrossFolders();
    void deleteNonEmpty();
    void deleteRecursive();
    void uploadWithoutOverwrite();
    void uploadWithOverwrite();
    void readOnlyAccount();
    void protectedFolder();
};

void FileSystemTest::listAndStat()
{
    Fixture fixture;
    const QList<Chomikuj::Entry> root = fixture.fs->list(QStringLiteral("/"));
    QCOMPARE(root.size(), 2);
    QCOMPARE(root.at(0).name, QStringLiteral("Music"));
    QVERIFY(root.at(0).isDir);
    QCOMPARE(root.at(1).name, QStringLiteral("root.txt"));
    QCOMPARE(root.at(1).sizeBytes, qint64(2048));

    const Chomikuj::Entry entry = fixture.fs->stat(QStringLiteral("/Music/A.mp3"));
    QCOMPARE(entry.id, qint64(2));
    QCOMPARE(entry.name, QStringLiteral("A.mp3"));
    QVERIFY(!entry.isDir);
}

void FileSystemTest::statIsCached()
{
    Fixture fixture;
    auto folderRequests = [&fixture] {
        int count = 0;
        for (const auto &request : fixture.fake->requests) {
            if (request.url.path() == QLatin1String("/api/v3/folders")) {
                ++count;
            }
        }
        return count;
    };

    const Chomikuj::Entry first = fixture.fs->stat(QStringLiteral("/Music/A.mp3"));
    QCOMPARE(first.id, qint64(2));
    const int afterFirst = folderRequests();
    QVERIFY(afterFirst >= 2); // root + Music

    const Chomikuj::Entry second = fixture.fs->stat(QStringLiteral("/Music/A.mp3"));
    QCOMPARE(second.id, qint64(2));
    QCOMPARE(folderRequests(), afterFirst);

    fixture.fs->clearCache();
    const Chomikuj::Entry third = fixture.fs->stat(QStringLiteral("/Music/A.mp3"));
    QCOMPARE(third.id, qint64(2));
    QVERIFY(folderRequests() > afterFirst);
}

void FileSystemTest::missing()
{
    Fixture fixture;
    try {
        fixture.fs->stat(QStringLiteral("/Nope"));
        QFAIL("expected not found");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_DOES_NOT_EXIST));
    }
}

void FileSystemTest::mkdir()
{
    Fixture fixture;
    fixture.fs->mkdir(QStringLiteral("/Music/New"));

    bool found = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.path() == QLatin1String("/api/v3/folders/create")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            QCOMPARE(body.value(QStringLiteral("FolderName")).toString(), QStringLiteral("New"));
            QCOMPARE(body.value(QStringLiteral("ParentId")).toInt(), 7);
            found = true;
        }
    }
    QVERIFY(found);
}

void FileSystemTest::rename()
{
    Fixture fixture;
    fixture.fs->rename(QStringLiteral("/Music/A.mp3"), QStringLiteral("/Music/B.mp3"), false);

    bool found = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.path() == QLatin1String("/api/v3/files/changeName")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            QCOMPARE(body.value(QStringLiteral("FileId")).toInt(), 2);
            QCOMPARE(body.value(QStringLiteral("FileName")).toString(), QStringLiteral("B.mp3"));
            found = true;
        }
    }
    QVERIFY(found);
}

void FileSystemTest::moveAcrossFolders()
{
    Fixture fixture;
    fixture.fs->rename(QStringLiteral("/Music/A.mp3"), QStringLiteral("/Music/Sub/A.mp3"), false);

    bool found = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.path() == QLatin1String("/api/v3/files/move")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            QCOMPARE(body.value(QStringLiteral("SourceFolderId")).toInt(), 7);
            QCOMPARE(body.value(QStringLiteral("TargetFolderId")).toInt(), 8);
            QCOMPARE(body.value(QStringLiteral("Files")).toArray().at(0).toInt(), 2);
            found = true;
        }
    }
    QVERIFY(found);
}

void FileSystemTest::deleteNonEmpty()
{
    Fixture fixture;
    try {
        fixture.fs->remove(QStringLiteral("/Music"), false, false);
        QFAIL("expected a non-empty folder error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_CANNOT_RMDIR));
    }
    for (const auto &request : fixture.fake->requests) {
        QVERIFY(request.url.path() != QLatin1String("/api/v3/files/delete"));
    }
}

void FileSystemTest::deleteRecursive()
{
    Fixture fixture;
    fixture.fs->remove(QStringLiteral("/Music"), false, true);

    bool deleted = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.path() == QLatin1String("/api/v3/files/delete")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            QCOMPARE(body.value(QStringLiteral("Folders")).toArray().at(0).toInt(), 7);
            deleted = true;
        }
    }
    QVERIFY(deleted);
}

void FileSystemTest::uploadWithoutOverwrite()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("x"), qint64(1));
    file.flush();

    Fixture fixture;
    try {
        fixture.fs->upload(file.fileName(), QStringLiteral("/Music/existing.bin"),
                           false, nullptr, [] { return false; });
        QFAIL("expected an exists error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_FILE_ALREADY_EXIST));
    }
    for (const auto &request : fixture.fake->requests) {
        QVERIFY(request.url.path() != QLatin1String("/api/v3/files/delete"));
        QVERIFY(request.url.path() != QLatin1String("/api/v3/files/upload/partialUpload"));
    }
}

void FileSystemTest::uploadWithOverwrite()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("x"), qint64(1));
    file.flush();

    Fixture fixture;
    QString temporaryName;
    bool deleted = false;
    bool renamed = false;

    fixture.fake->handler = [&](const Chomikuj::HttpRequest &request) -> Chomikuj::HttpResponse {
        if (request.url.path() == QLatin1String("/api/v3/folders")) {
            const QString parent =
                QUrlQuery(request.url).queryItemValue(QStringLiteral("Parent"));
            if (parent == QLatin1String("0")) {
                return jsonResponse(200, listing("{\"Id\":7,\"Name\":\"Music\"}", ""));
            }
            QByteArray files = "{\"FileId\":3,\"FileName\":\"existing\","
                              "\"FileType\":\"bin\",\"Size\":1}";
            if (!temporaryName.isEmpty()) {
                QString base;
                QString extension;
                Chomikuj::splitName(temporaryName, &base, &extension);
                files += ",{\"FileId\":99,\"FileName\":\"" + base.toUtf8()
                    + "\",\"FileType\":\"" + extension.toUtf8() + "\",\"Size\":1}";
            }
            return jsonResponse(200, listing("", files));
        }
        if (request.url.path() == QLatin1String("/api/v3/files/upload/partialUpload")) {
            temporaryName = QJsonDocument::fromJson(request.body)
                                .object()
                                .value(QStringLiteral("Name"))
                                .toString();
            return jsonResponse(200, "{\"Code\":0,\"Url\":\"\",\"Chunk\":0,"
                                       "\"UploadCompleted\":true}");
        }
        if (request.url.path() == QLatin1String("/api/v3/files/delete")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if (body.value(QStringLiteral("Files")).toArray().at(0).toInt() != 3) {
                return jsonResponse(400, "{\"Message\":\"wrong delete target\"}");
            }
            deleted = true;
            return jsonResponse(200, "{}");
        }
        if (request.url.path() == QLatin1String("/api/v3/files/changeName")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if (body.value(QStringLiteral("FileId")).toInt() != 99
                || body.value(QStringLiteral("FileName")).toString()
                    != QLatin1String("existing.bin")) {
                return jsonResponse(400, "{\"Message\":\"wrong rename target\"}");
            }
            renamed = true;
            return jsonResponse(200, "{\"Code\":0}");
        }
        return jsonResponse(500, "{}");
    };

    fixture.fs->upload(file.fileName(), QStringLiteral("/Music/existing.bin"),
                       true, nullptr, [] { return false; });

    QVERIFY(temporaryName.startsWith(QLatin1String("existing.__kio_tmp_")));
    QVERIFY(temporaryName.endsWith(QLatin1String(".bin")));
    QVERIFY(deleted);
    QVERIFY(renamed);
}

void FileSystemTest::readOnlyAccount()
{
    Fixture fixture;
    ChomikujFileSystem readOnly(*fixture.client, 123, 123, true);
    try {
        readOnly.mkdir(QStringLiteral("/Music/New"));
        QFAIL("expected a read-only error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_ACCESS_DENIED));
    }
    try {
        readOnly.remove(QStringLiteral("/Music/A.mp3"), true, false);
        QFAIL("expected a read-only error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_ACCESS_DENIED));
    }
}

void FileSystemTest::protectedFolder()
{
    Fixture fixture;
    bool passwordRequested = false;
    int folderListCalls = 0;
    fixture.fake->handler = [&](const Chomikuj::HttpRequest &request) -> Chomikuj::HttpResponse {
        const QString path = request.url.path();
        if (path == QLatin1String("/api/v3/folders")) {
            const QString parent = QUrlQuery(request.url).queryItemValue(QStringLiteral("Parent"));
            if (parent == QLatin1String("0")) {
                return jsonResponse(200,
                                    listing("{\"Id\":7,\"Name\":\"Music\",\"HasPassword\":true}",
                                            ""));
            }
            if (parent == QLatin1String("7")) {
                ++folderListCalls;
                if (folderListCalls == 1) {
                    return jsonResponse(401, "{\"Code\":12,\"Message\":\"Folder password\"}");
                }
                return jsonResponse(200,
                                    listing("", "{\"FileId\":2,\"FileName\":\"A\","
                                                "\"FileType\":\"mp3\",\"Size\":1}"));
            }
        }
        if (path == QLatin1String("/api/v3/folders/password")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if (body.value(QStringLiteral("FolderId")).toInt() != 7
                || body.value(QStringLiteral("Password")).toString() != QLatin1String("secret")) {
                return jsonResponse(400, "{\"Message\":\"bad folder password body\"}");
            }
            passwordRequested = true;
            return jsonResponse(200, "{}");
        }
        return jsonResponse(500, "{}");
    };

    int prompts = 0;
    fixture.fs->setPasswordProvider([&](qint64 folderId, QString *password, const QString &) {
        if (folderId != 7) {
            return false;
        }
        ++prompts;
        *password = QStringLiteral("secret");
        return true;
    });

    const QList<Chomikuj::Entry> entries = fixture.fs->list(QStringLiteral("/Music"));
    QCOMPARE(prompts, 1);
    QVERIFY(passwordRequested);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().name, QStringLiteral("A.mp3"));
}

QTEST_GUILESS_MAIN(FileSystemTest)

#include "tst_filesystem.moc"

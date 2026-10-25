/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "faketransport.h"

#include <KIO/Global>

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtTest>

using Chomikuj::ChomikujClient;
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

QByteArray folderPage(const QByteArray &folders, const QByteArray &files, bool next)
{
    return "{\"Code\":0,\"Folders\":[" + folders + "],\"Files\":[" + files
        + "],\"IsNextPageAvailable\":" + (next ? "true" : "false") + "}";
}

struct Fixture
{
    FakeTransport *fake = nullptr;
    std::unique_ptr<ChomikujClient> client;

    explicit Fixture(const QString &baseUrl = QStringLiteral("https://example.test"))
    {
        auto transport = std::make_unique<FakeTransport>();
        fake = transport.get();
        client = std::make_unique<ChomikujClient>(std::move(transport),
                                                  QStringLiteral("dev-id"),
                                                  QStringLiteral("TestDevice"),
                                                  baseUrl);
    }
};

} // namespace

class ClientTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void login();
    void listPagination();
    void businessError();
    void httpError();
    void retriesIdempotent();
    void download();
    void downloadResume();
    void downloadResumeRejectsIgnoredRange();
    void accountSearch();
    void publicListing();
    void folderPassword();
};

void ClientTest::login()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if (body.value(QStringLiteral("AccountName")).toString() != QLatin1String("user")
                || body.value(QStringLiteral("Password")).toString() != QLatin1String("pass")) {
                return jsonResponse(400, "{\"Message\":\"bad body\"}");
            }
            if (request.headers.value("Token").isEmpty()) {
                return jsonResponse(400, "{\"Message\":\"missing token\"}");
            }
            if (!request.headers.value("User-Agent").startsWith("android/3.8.4 (dev-id; TestDevice)")) {
                return jsonResponse(400, "{\"Message\":\"bad user agent\"}");
            }
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\","
                                       "\"AvatarUrl\":\"https://avatar\"}");
        }
        return jsonResponse(500, "{}");
    };

    const Chomikuj::LoginResult result = fixture.client->login(QStringLiteral("user"),
                                                               QStringLiteral("pass"));
    QCOMPARE(result.accountId, QStringLiteral("42"));
    QCOMPARE(result.accountName, QStringLiteral("user"));
    QCOMPARE(result.apiKey, QStringLiteral("KEY"));
    QCOMPARE(fixture.client->accountId(), QStringLiteral("42"));
    QVERIFY(fixture.client->hasApiKey());
    QCOMPARE(fixture.fake->requests.size(), 1);
}

void ClientTest::listPagination()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        return jsonResponse(500, "{}");
    };
    fixture.client->login(QStringLiteral("user"), QStringLiteral("pass"));

    int pageCalls = 0;
    fixture.fake->handler = [&](const Chomikuj::HttpRequest &request) -> Chomikuj::HttpResponse {
        if (!request.url.path().startsWith(QLatin1String("/api/v3/folders"))) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\",\"ApiKey\":\"KEY\"}");
        }
        if (request.headers.value("Api-Key") != QByteArray("KEY")) {
            return jsonResponse(400, "{\"Message\":\"missing api key\"}");
        }
        const QUrlQuery query(request.url);
        ++pageCalls;
        if (query.queryItemValue(QStringLiteral("page")) == QLatin1String("1")) {
            if (query.queryItemValue(QStringLiteral("Parent")) != QLatin1String("0")) {
                return jsonResponse(400, "{\"Message\":\"bad parent\"}");
            }
            return jsonResponse(200,
                                folderPage("{\"Id\":7,\"Name\":\"Music\",\"HasPassword\":false}",
                                           "{\"FileId\":1,\"FileName\":\"f\",\"FileType\":\"txt\","
                                           "\"Size\":2,\"MediaType\":\"Document\",\"IsHidden\":true}",
                                           true));
        }
        if (query.queryItemValue(QStringLiteral("page")) != QLatin1String("2")) {
            return jsonResponse(400, "{\"Message\":\"bad page\"}");
        }
        return jsonResponse(200,
                            folderPage("",
                                       "{\"FileId\":2,\"FileName\":\"g\",\"FileType\":\"bin\","
                                       "\"Size\":1,\"IsHidden\":false}",
                                       false));
    };

    const QList<Chomikuj::Entry> entries = fixture.client->listFolder(0);
    QCOMPARE(pageCalls, 2);
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).name, QStringLiteral("Music"));
    QVERIFY(entries.at(0).isDir);
    QCOMPARE(entries.at(1).name, QStringLiteral("f.txt"));
    QCOMPARE(entries.at(1).sizeBytes, qint64(2048));
    QCOMPARE(entries.at(2).name, QStringLiteral("g.bin"));
    QCOMPARE(entries.at(2).sizeBytes, qint64(1024));
}

void ClientTest::businessError()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &) {
        return jsonResponse(200, "{\"Code\":5,\"Message\":\"NameExistsAtDestination\"}");
    };

    try {
        fixture.client->createFolder(QStringLiteral("x"), 0);
        QFAIL("expected an error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.businessCode(), 5);
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_FILE_ALREADY_EXIST));
    }
}

void ClientTest::httpError()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &) {
        return jsonResponse(404, "{\"Code\":11,\"Message\":\"No folder\"}");
    };

    try {
        fixture.client->listFolderPage(99, 1);
        QFAIL("expected an error");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.httpStatus(), 404);
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_DOES_NOT_EXIST));
        QCOMPARE(error.message(), QStringLiteral("No folder"));
    }
}

void ClientTest::retriesIdempotent()
{
    qputenv("CHOMIKUJ_RETRY_BASE_MS", "1");
    qputenv("CHOMIKUJ_RETRY_MAX_MS", "1");
    Fixture fixture;
    int calls = 0;
    fixture.fake->handler = [&](const Chomikuj::HttpRequest &) {
        ++calls;
        if (calls == 1) {
            return jsonResponse(500, "{}");
        }
        return jsonResponse(200, folderPage("", "", false));
    };

    const QList<Chomikuj::Entry> entries = fixture.client->listFolder(0);
    QVERIFY(entries.isEmpty());
    QCOMPARE(calls, 2);
}

void ClientTest::download()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/files/download")) {
            return jsonResponse(200, "{\"Code\":0,\"FileUrl\":\"https://cdn.test/file\"}");
        }
        if (request.url.host() == QLatin1String("cdn.test")) {
            HttpResponse response;
            response.status = 200;
            response.headers.insert("Content-Length", "11");
            response.body = "hello world";
            return response;
        }
        return jsonResponse(500, "{}");
    };

    QByteArray received;
    QList<qint64> progress;
    fixture.client->downloadFile(
        5,
        [&](const QByteArray &chunk) {
            received += chunk;
            return true;
        },
        [&](qint64 current, qint64 total) {
            progress.append(current);
            if (total >= 0) {
                QCOMPARE(total, qint64(11));
            }
        },
        [] {
            return false;
        });

    QCOMPARE(received, QByteArray("hello world"));
    QVERIFY(!progress.isEmpty());
}

void ClientTest::downloadResume()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/files/download")) {
            return jsonResponse(200, "{\"Code\":0,\"FileUrl\":\"https://cdn.test/file\"}");
        }
        if (request.url.host() == QLatin1String("cdn.test")) {
            const QByteArray range = request.headers.value("Range");
            if (range == QByteArray("bytes=6-6")) {
                HttpResponse response;
                response.status = 206;
                response.headers.insert("Content-Length", "1");
                response.headers.insert("Content-Range", "bytes 6-6/11");
                response.body = "w";
                return response;
            }
            if (range != QByteArray("bytes=6-")) {
                return jsonResponse(400, "{}");
            }
            HttpResponse response;
            response.status = 206;
            response.headers.insert("Content-Length", "5");
            response.headers.insert("Content-Range", "bytes 6-10/11");
            response.body = "world";
            return response;
        }
        return jsonResponse(500, "{}");
    };

    QByteArray received;
    QList<qint64> progress;
    fixture.client->downloadFile(
        5,
        [&](const QByteArray &chunk) {
            received += chunk;
            return true;
        },
        [&](qint64 current, qint64 total) {
            progress.append(current);
            if (total >= 0) {
                QCOMPARE(total, qint64(11));
            }
        },
        [] {
            return false;
        },
        6);

    QCOMPARE(received, QByteArray("world"));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.first(), qint64(6));
}

void ClientTest::downloadResumeRejectsIgnoredRange()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/files/download")) {
            return jsonResponse(200, "{\"Code\":0,\"FileUrl\":\"https://cdn.test/file\"}");
        }
        if (request.url.host() == QLatin1String("cdn.test")) {
            HttpResponse response;
            response.status = 200;
            response.headers.insert("Content-Length", "11");
            response.body = "hello world";
            return response;
        }
        return jsonResponse(500, "{}");
    };

    try {
        fixture.client->downloadFile(
            5,
            [](const QByteArray &) {
                return true;
            },
            nullptr,
            [] {
                return false;
            },
            6);
        QFAIL("expected ERR_CANNOT_RESUME");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_CANNOT_RESUME));
    }
}

void ClientTest::accountSearch()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        if (request.url.path() == QLatin1String("/api/v3/account/search")) {
            const QUrlQuery query(request.url);
            if (query.queryItemValue(QStringLiteral("Query")) != QLatin1String("Other")) {
                return jsonResponse(400, "{\"Message\":\"bad query\"}");
            }
            return jsonResponse(200,
                                "{\"IsNextPageAvailable\":false,\"Results\":["
                                "{\"AccountId\":123,\"AccountName\":\"Other\"},"
                                "{\"AccountId\":456,\"AccountName\":\"Other2\"}]}");
        }
        return jsonResponse(500, "{}");
    };
    fixture.client->login(QStringLiteral("user"), QStringLiteral("pass"));

    const QList<Chomikuj::AccountSearchItem> results =
        fixture.client->searchAccounts(QStringLiteral("Other"));
    QCOMPARE(results.size(), 2);
    QCOMPARE(results.at(0).id, qint64(123));
    QCOMPARE(results.at(0).name, QStringLiteral("Other"));
}

void ClientTest::publicListing()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        if (request.url.path() == QLatin1String("/api/v3/folders")) {
            const QUrlQuery query(request.url);
            if (query.queryItemValue(QStringLiteral("AccountId")) != QLatin1String("123")) {
                return jsonResponse(400, "{\"Message\":\"missing account id\"}");
            }
            return jsonResponse(200, folderPage("", "", false));
        }
        return jsonResponse(500, "{}");
    };
    fixture.client->login(QStringLiteral("user"), QStringLiteral("pass"));

    const QList<Chomikuj::Entry> entries = fixture.client->listFolder(0, 123);
    QVERIFY(entries.isEmpty());
}

void ClientTest::folderPassword()
{
    Fixture fixture;
    fixture.fake->handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        if (request.url.path() == QLatin1String("/api/v3/folders/password")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            if (body.value(QStringLiteral("AccountId")).toInt() != 123
                || body.value(QStringLiteral("FolderId")).toInt() != 7
                || body.value(QStringLiteral("Password")).toString() != QLatin1String("secret")) {
                return jsonResponse(400, "{\"Message\":\"bad password body\"}");
            }
            return jsonResponse(200, "{}");
        }
        return jsonResponse(500, "{}");
    };
    fixture.client->login(QStringLiteral("user"), QStringLiteral("pass"));
    fixture.client->folderPassword(123, 7, QStringLiteral("secret"));
}

QTEST_GUILESS_MAIN(ClientTest)

#include "tst_client.moc"

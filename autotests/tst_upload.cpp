/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujclient.h"
#include "core/chomikujerror.h"
#include "core/uploader.h"
#include "faketransport.h"

#include <QFile>
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

struct UploadFixture
{
    FakeTransport *fake = nullptr;
    std::unique_ptr<ChomikujClient> client;
    qint64 size = 0;
    qint64 stored = 0;
    bool listed = false;
    bool failFirstDataChunk = false;

    UploadFixture()
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
    }

    HttpResponse handle(const Chomikuj::HttpRequest &request)
    {
        if (request.url.path() == QLatin1String("/api/v3/account/login")) {
            return jsonResponse(200, "{\"Code\":0,\"AccountId\":\"42\","
                                       "\"AccountName\":\"user\",\"ApiKey\":\"KEY\"}");
        }
        if (request.url.path() == QLatin1String("/api/v3/files/upload/partialUpload")) {
            const qint64 chunk = listed ? size : stored;
            return jsonResponse(200,
                                "{\"Code\":0,\"Url\":\"https://upload.test/target\","
                                "\"Chunk\":" + QByteArray::number(chunk)
                                    + ",\"UploadCompleted\":" + (listed ? "true" : "false") + "}");
        }
        if (request.url.path() == QLatin1String("/api/v3/folders")) {
            if (!listed) {
                return jsonResponse(200, "{\"Code\":0,\"Folders\":[],\"Files\":[],"
                                           "\"IsNextPageAvailable\":false}");
            }
            return jsonResponse(200,
                                "{\"Code\":0,\"Folders\":[],\"Files\":["
                                "{\"FileId\":1,\"FileName\":\"probe\",\"FileType\":\"bin\","
                                "\"Size\":" + QByteArray::number(size / 1024)
                                    + ",\"IsHidden\":false}],"
                                      "\"IsNextPageAvailable\":false}");
        }
        if (request.url.host() == QLatin1String("upload.test")) {
            const QByteArray range = request.headers.value("File-Range");
            const int dash = range.indexOf('-');
            const qint64 start = range.left(dash).toLongLong();
            const qint64 end = range.mid(dash + 1).toLongLong();
            if (start == 0 && end > 0 && failFirstDataChunk) {
                failFirstDataChunk = false;
                return jsonResponse(409, "{}");
            }
            if (start == end) {
                if (stored >= size) {
                    listed = true;
                    return jsonResponse(200, "{}");
                }
                return jsonResponse(206, "{}");
            }
            stored = end;
            return jsonResponse(stored >= size ? 206 : 206, "{}");
        }
        return jsonResponse(500, "{}");
    }
};

} // namespace

class UploadTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void oneChunk();
    void zeroLength();
    void multipleChunks();
    void alreadyComplete();
    void resetAfterConflict();
    void filenameEscaping();
    void cancel();
};

void UploadTest::initTestCase()
{
    qputenv("CHOMIKUJ_RETRY_BASE_MS", "1");
    qputenv("CHOMIKUJ_RETRY_MAX_MS", "1");
}

void UploadTest::oneChunk()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("hello world"), qint64(11));
    file.flush();

    UploadFixture fixture;
    fixture.size = 11;
    QList<qint64> progress;
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("probe.bin"),
                               [&](qint64 current, qint64 total) {
                                   QCOMPARE(total, qint64(11));
                                   progress.append(current);
                               },
                               [] {
                                   return false;
                               });

    QVERIFY(fixture.listed);
    QCOMPARE(progress.last(), qint64(11));
    bool sawMultipart = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.host() == QLatin1String("upload.test")) {
            QVERIFY(request.body.contains("name=\"files[]\""));
            QVERIFY(request.body.contains("filename=\"probe.bin\""));
            const QByteArray range = request.headers.value("File-Range");
            if (range != QByteArray("11-11")) {
                QVERIFY(range.startsWith("0-"));
            }
            sawMultipart = true;
        }
    }
    QVERIFY(sawMultipart);
}

void UploadTest::zeroLength()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    file.flush();

    UploadFixture fixture;
    fixture.size = 0;
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("empty.bin"),
                               nullptr, [] { return false; });
    QVERIFY(fixture.listed);
}

void UploadTest::multipleChunks()
{
    const QByteArray content(1024 * 1024 + 5, 'x');
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(content), qint64(content.size()));
    file.flush();

    UploadFixture fixture;
    fixture.size = content.size();
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("probe.bin"),
                               nullptr, [] { return false; });

    QVERIFY(fixture.listed);
    QCOMPARE(fixture.stored, fixture.size);
    QList<QByteArray> ranges;
    for (const auto &request : fixture.fake->requests) {
        const QByteArray range = request.headers.value("File-Range");
        if (request.url.host() == QLatin1String("upload.test") && !range.isEmpty()) {
            ranges.append(range);
        }
    }
    QVERIFY(ranges.size() >= 3);
}

void UploadTest::alreadyComplete()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("abc"), qint64(3));
    file.flush();

    UploadFixture fixture;
    fixture.size = 3;
    fixture.listed = true;
    const int before = fixture.fake->requests.size();
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("probe.bin"),
                               nullptr, [] { return false; });
    QCOMPARE(fixture.fake->requests.size(), before + 1);
}

void UploadTest::resetAfterConflict()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("0123456789"), qint64(10));
    file.flush();

    UploadFixture fixture;
    fixture.size = 10;
    fixture.failFirstDataChunk = true;
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("probe.bin"),
                               nullptr, [] { return false; });

    QVERIFY(fixture.listed);
    QCOMPARE(fixture.stored, qint64(10));
}

void UploadTest::filenameEscaping()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("abc"), qint64(3));
    file.flush();

    UploadFixture fixture;
    fixture.size = 3;
    fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("a?b:c.bin"),
                               nullptr, [] { return false; });

    bool found = false;
    for (const auto &request : fixture.fake->requests) {
        if (request.url.host() == QLatin1String("upload.test")) {
            QVERIFY(request.body.contains("filename=\"a_b_c.bin\""));
            found = true;
        }
    }
    QVERIFY(found);
}

void UploadTest::cancel()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("abc"), qint64(3));
    file.flush();

    UploadFixture fixture;
    fixture.size = 3;
    try {
        fixture.client->uploadFile(file.fileName(), 5, QStringLiteral("probe.bin"),
                                   nullptr, [] { return true; });
        QFAIL("expected cancellation");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_USER_CANCELED));
    }
}

QTEST_GUILESS_MAIN(UploadTest)

#include "tst_upload.moc"

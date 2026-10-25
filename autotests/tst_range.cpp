/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujerror.h"
#include "core/rangereader.h"
#include "faketransport.h"

#include <KIO/Global>

#include <QtTest>

using Chomikuj::HttpResponse;
using Chomikuj::RangeReader;

namespace
{

HttpResponse response(int status, const QByteArray &body = {},
                      const QList<QPair<QByteArray, QByteArray>> &headers = {})
{
    HttpResponse result;
    result.status = status;
    result.body = body;
    for (const auto &header : headers) {
        result.headers.insert(header.first, header.second);
    }
    return result;
}

} // namespace

class RangeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void probe200Length();
    void probeRange();
    void probeFallback();
    void probeRefreshesUrl();
    void readUsesClosedRange();
    void readClampsAtEof();
    void readPastEofDoesNotRequest();
    void readZeroLengthDoesNotRequest();
    void readTruncatesOverlongResponse();
    void readShortResponse();
    void readLearnsSizeFromContentRange();
    void readLearnsSizeFrom200();
    void readRejects200AtOffset();
    void readRejectsWrongContentRange();
    void readEmptyBeforeEof();
    void read416LearnsSizeAndEof();
    void readRefreshesUrl();
    void readRetriesServerError();
    void readCancellation();
};

void RangeTest::probe200Length()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &request) {
        Q_ASSERT(request.method == QByteArrayLiteral("GET"));
        Q_ASSERT(request.headers.value("Range") == QByteArray("bytes=0-0"));
        Q_ASSERT(request.headers.value("Accept-Encoding") == QByteArray("identity"));
        return response(200, {}, {{"Content-Length", "12345"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1024);
    QCOMPARE(reader.probeExactSize({}, {}), qint64(12345));
    QCOMPARE(reader.size(), qint64(12345));
    QCOMPARE(fake.requests.size(), 1);
    QCOMPARE(fake.requests.first().headers.value("Range"),
             QByteArrayLiteral("bytes=0-0"));
}

void RangeTest::probeRange()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &request) {
        Q_ASSERT(request.method == QByteArrayLiteral("GET"));
        Q_ASSERT(request.headers.value("Range") == QByteArray("bytes=0-0"));
        return response(206, "x", {{"Content-Range", "bytes 0-0/4321"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1024);
    QCOMPARE(reader.probeExactSize({}, {}), qint64(4321));
    QCOMPARE(fake.requests.size(), 1);
    QCOMPARE(fake.requests.first().headers.value("Range"),
             QByteArrayLiteral("bytes=0-0"));
}

void RangeTest::probeFallback()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(200, "x");
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       2048);
    QCOMPARE(reader.probeExactSize({}, {}), qint64(2048));
    QVERIFY(!reader.sizeKnown());
}

void RangeTest::probeRefreshesUrl()
{
    FakeTransport fake;
    int calls = 0;
    fake.handler = [&](const Chomikuj::HttpRequest &request) {
        ++calls;
        if (request.url == QUrl(QStringLiteral("https://cdn/old"))) {
            return response(403);
        }
        Q_ASSERT(request.url == QUrl(QStringLiteral("https://cdn/new")));
        return response(200, {}, {{"Content-Length", "777"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/old"),
                       1024);
    const qint64 size = reader.probeExactSize(
        [] {
            return QStringLiteral("https://cdn/new");
        },
        {});
    QCOMPARE(size, qint64(777));
    QCOMPARE(reader.url(), QStringLiteral("https://cdn/new"));
    QCOMPARE(calls, 2);
    QCOMPARE(fake.requests.at(0).url, QUrl(QStringLiteral("https://cdn/old")));
    QCOMPARE(fake.requests.at(1).url, QUrl(QStringLiteral("https://cdn/new")));
}

void RangeTest::readUsesClosedRange()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &request) {
        Q_ASSERT(request.method == QByteArrayLiteral("GET"));
        Q_ASSERT(request.headers.value("Range") == QByteArray("bytes=100-102"));
        Q_ASSERT(request.headers.value("Accept-Encoding") == QByteArray("identity"));
        return response(206, "abc", {{"Content-Range", "bytes 100-102/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    QCOMPARE(reader.read(100, 3, {}, {}), QByteArray("abc"));
    QCOMPARE(fake.requests.first().headers.value("Range"),
             QByteArrayLiteral("bytes=100-102"));
    QCOMPARE(fake.requests.first().headers.value("Accept-Encoding"),
             QByteArrayLiteral("identity"));
}

void RangeTest::readClampsAtEof()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &request) {
        Q_ASSERT(request.headers.value("Range") == QByteArray("bytes=98-99"));
        return response(206, "yz", {{"Content-Range", "bytes 98-99/100"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       100, 100);
    QCOMPARE(reader.read(98, 50, {}, {}), QByteArray("yz"));
    QCOMPARE(fake.requests.first().headers.value("Range"),
             QByteArrayLiteral("bytes=98-99"));
}

void RangeTest::readPastEofDoesNotRequest()
{
    FakeTransport fake;
    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       100, 100);
    QCOMPARE(reader.read(100, 10, {}, {}), QByteArray());
    QVERIFY(fake.requests.isEmpty());
}

void RangeTest::readZeroLengthDoesNotRequest()
{
    FakeTransport fake;
    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       100, 100);
    QCOMPARE(reader.read(0, 0, {}, {}), QByteArray());
    QVERIFY(fake.requests.isEmpty());
}

void RangeTest::readTruncatesOverlongResponse()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(206, "0123456789", {{"Content-Range", "bytes 0-9/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    QCOMPARE(reader.read(0, 3, {}, {}), QByteArray("012"));
}

void RangeTest::readShortResponse()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(206, "ab", {{"Content-Range", "bytes 10-11/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    QCOMPARE(reader.read(10, 10, {}, {}), QByteArray("ab"));
}

void RangeTest::readLearnsSizeFromContentRange()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(206, "abcde", {{"Content-Range", "bytes 0-4/5000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       0);
    QCOMPARE(reader.read(0, 5, {}, {}), QByteArray("abcde"));
    QCOMPARE(reader.size(), qint64(5000));
}

void RangeTest::readLearnsSizeFrom200()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(200, "abc", {{"Content-Length", "5000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       0);
    QCOMPARE(reader.read(0, 3, {}, {}), QByteArray("abc"));
    QCOMPARE(reader.size(), qint64(5000));
}

void RangeTest::readRejects200AtOffset()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(200, "wrong data", {{"Content-Length", "10"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    try {
        reader.read(5, 3, {}, {});
        QFAIL("expected ERR_CANNOT_SEEK");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_CANNOT_SEEK));
    }
}

void RangeTest::readRejectsWrongContentRange()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(206, "abc", {{"Content-Range", "bytes 0-2/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    try {
        reader.read(10, 3, {}, {});
        QFAIL("expected ERR_CANNOT_SEEK");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_CANNOT_SEEK));
    }
}

void RangeTest::readEmptyBeforeEof()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(206, {}, {{"Content-Range", "bytes 10-10/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       1000, 1000);
    try {
        reader.read(10, 5, {}, {});
        QFAIL("expected ERR_CANNOT_READ");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_CANNOT_READ));
    }
}

void RangeTest::read416LearnsSizeAndEof()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &) {
        return response(416, {}, {{"Content-Range", "bytes */100"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       0);
    QCOMPARE(reader.read(100, 5, {}, {}), QByteArray());
    QCOMPARE(reader.size(), qint64(100));
    QVERIFY(reader.sizeKnown());
}

void RangeTest::readRefreshesUrl()
{
    FakeTransport fake;
    fake.handler = [](const Chomikuj::HttpRequest &request) {
        if (request.url == QUrl(QStringLiteral("https://cdn/old"))) {
            return response(403);
        }
        Q_ASSERT(request.url == QUrl(QStringLiteral("https://cdn/new")));
        Q_ASSERT(request.headers.value("Range") == QByteArray("bytes=10-12"));
        return response(206, "abc", {{"Content-Range", "bytes 10-12/1000"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/old"),
                       1000, 1000);
    const QByteArray data = reader.read(
        10, 3,
        [] {
            return QStringLiteral("https://cdn/new");
        },
        {});
    QCOMPARE(data, QByteArray("abc"));
    QCOMPARE(fake.requests.size(), 2);
    QCOMPARE(fake.requests.at(0).url, QUrl(QStringLiteral("https://cdn/old")));
    QCOMPARE(fake.requests.at(1).url, QUrl(QStringLiteral("https://cdn/new")));
}

void RangeTest::readRetriesServerError()
{
    qputenv("CHOMIKUJ_RETRY_BASE_MS", "1");
    qputenv("CHOMIKUJ_RETRY_MAX_MS", "1");
    FakeTransport fake;
    int calls = 0;
    fake.handler = [&](const Chomikuj::HttpRequest &) {
        if (++calls == 1) {
            return response(500);
        }
        return response(206, "abc", {{"Content-Range", "bytes 0-2/3"}});
    };

    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       3, 3);
    QCOMPARE(reader.read(0, 3, {}, {}), QByteArray("abc"));
    QCOMPARE(calls, 2);
    qunsetenv("CHOMIKUJ_RETRY_BASE_MS");
    qunsetenv("CHOMIKUJ_RETRY_MAX_MS");
}

void RangeTest::readCancellation()
{
    FakeTransport fake;
    RangeReader reader(fake, QStringLiteral("agent"), QStringLiteral("https://cdn/file"),
                       100, 100);
    try {
        reader.read(0, 10, {}, [] {
            return true;
        });
        QFAIL("expected ERR_USER_CANCELED");
    } catch (const Chomikuj::Error &error) {
        QCOMPARE(error.kioCode(), static_cast<int>(KIO::ERR_USER_CANCELED));
    }
    QVERIFY(fake.requests.isEmpty());
}

QTEST_GUILESS_MAIN(RangeTest)

#include "tst_range.moc"

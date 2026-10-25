/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * Integration test for the KIO FileJob path, using the same call pattern as
 * kio-fuse: open, seek, read, close for every range.  The Chomikuj backend is
 * replaced by a small local HTTP mock, so the test needs no credentials and no
 * network access.
 */

#include <KIO/FileJob>
#include <KIO/Global>
#include <KIO/Job>
#include <KIO/ListJob>
#include <KIO/StatJob>
#include <KIO/TransferJob>
#include <KProtocolManager>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>

namespace
{

constexpr int s_timeoutMs = 30000;

QByteArray payload()
{
    QByteArray data;
    data.reserve(123456);
    for (int i = 0; i < 123456; ++i) {
        data.append(static_cast<char>((i * 31 + 7) & 0xff));
    }
    return data;
}

class MockServer : public QObject
{
public:
    explicit MockServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_payload(payload())
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MockServer::acceptConnection);
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    bool isValid() const
    {
        return m_server.isListening();
    }

    quint16 port() const
    {
        return m_server.serverPort();
    }

    const QByteArray &content() const
    {
        return m_payload;
    }

    const QList<QByteArray> &ranges() const
    {
        return m_ranges;
    }

    int downloadUrlCalls() const
    {
        return m_downloadUrlCalls;
    }

private:
    void acceptConnection()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                readRequest(socket);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void readRequest(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();

        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }

        const QByteArray header = buffer.left(headerEnd);
        const QList<QByteArray> lines = header.split('\n');
        if (lines.isEmpty()) {
            respond(socket, 400, {}, {});
            return;
        }

        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        if (requestLine.size() < 2) {
            respond(socket, 400, {}, {});
            return;
        }

        const QByteArray method = requestLine.at(0);
        const QByteArray target = requestLine.at(1);

        QHash<QByteArray, QByteArray> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon > 0) {
                headers.insert(line.left(colon).trimmed().toLower(),
                               line.mid(colon + 1).trimmed());
            }
        }

        const int contentLength = headers.value("content-length").toInt();
        if (buffer.size() < headerEnd + 4 + contentLength) {
            return;
        }
        buffer.remove(0, headerEnd + 4 + contentLength);

        handleRequest(socket, method, target, headers);
    }

    void handleRequest(QTcpSocket *socket, const QByteArray &method,
                       const QByteArray &target, const QHash<QByteArray, QByteArray> &headers)
    {
        const QUrl url = QUrl::fromEncoded(target);
        const QString path = url.path();
        const QUrlQuery query(url);

        if (method == QByteArrayLiteral("POST")
            && path == QLatin1String("/api/v3/account/login")) {
            const QJsonObject body{
                {QStringLiteral("Code"), 0},
                {QStringLiteral("AccountId"), QStringLiteral("42")},
                {QStringLiteral("AccountName"), QStringLiteral("user")},
                {QStringLiteral("ApiKey"), QStringLiteral("KEY")},
            };
            respondJson(socket, body);
            return;
        }

        if (method == QByteArrayLiteral("GET")
            && path == QLatin1String("/api/v3/account/search")) {
            const QJsonArray results{
                QJsonObject{{QStringLiteral("AccountId"), 99},
                            {QStringLiteral("AccountName"), QStringLiteral("other")}},
            };
            respondJson(socket, QJsonObject{{QStringLiteral("IsNextPageAvailable"), false},
                                            {QStringLiteral("Results"), results}});
            return;
        }

        if (method == QByteArrayLiteral("GET")
            && path == QLatin1String("/api/v3/folders")) {
            const QString parent = query.queryItemValue(QStringLiteral("Parent"));
            if (parent != QLatin1String("0")) {
                respondJson(socket, QJsonObject{{QStringLiteral("Code"), 0},
                                                {QStringLiteral("Folders"), QJsonArray()},
                                                {QStringLiteral("Files"), QJsonArray()}});
                return;
            }
            const QJsonArray files{
                QJsonObject{
                    {QStringLiteral("FileId"), 123},
                    {QStringLiteral("FileName"), QStringLiteral("movie")},
                    {QStringLiteral("FileType"), QStringLiteral("mkv")},
                    {QStringLiteral("Size"), 120}, // floored KB
                    {QStringLiteral("MediaType"), QStringLiteral("Video")},
                },
                QJsonObject{
                    {QStringLiteral("FileId"), 124},
                    {QStringLiteral("FileName"), QStringLiteral("notes")},
                    {QStringLiteral("FileType"), QStringLiteral("txt")},
                    {QStringLiteral("Size"), 1},
                    {QStringLiteral("MediaType"), QStringLiteral("Document")},
                },
            };
            respondJson(socket, QJsonObject{{QStringLiteral("Code"), 0},
                                            {QStringLiteral("Folders"), QJsonArray()},
                                            {QStringLiteral("Files"), files},
                                            {QStringLiteral("IsNextPageAvailable"), false}});
            return;
        }

        if (method == QByteArrayLiteral("GET")
            && path == QLatin1String("/api/v3/files/download")) {
            ++m_downloadUrlCalls;
            const QJsonObject body{
                {QStringLiteral("Code"), 0},
                {QStringLiteral("FileUrl"),
                 QStringLiteral("http://127.0.0.1:%1/blob").arg(m_server.serverPort())},
            };
            respondJson(socket, body);
            return;
        }

        if (method == QByteArrayLiteral("HEAD") && path == QLatin1String("/blob")) {
            respond(socket, 200,
                    {{"Content-Length", QByteArray::number(m_payload.size())},
                     {"Accept-Ranges", "bytes"}},
                    {});
            return;
        }

        if (method == QByteArrayLiteral("GET") && path == QLatin1String("/blob")) {
            const QByteArray range = headers.value("range");
            m_ranges.append(range);
            if (range.isEmpty()) {
                respond(socket, 200,
                        {{"Content-Length", QByteArray::number(m_payload.size())}},
                        m_payload);
                return;
            }

            static const QRegularExpression expression(
                QStringLiteral("^bytes=(\\d+)-(\\d*)$"));
            const QRegularExpressionMatch match =
                expression.match(QString::fromLatin1(range));
            if (!match.hasMatch()) {
                respond(socket, 416, {}, {});
                return;
            }

            bool startOk = false;
            const qint64 start = match.captured(1).toLongLong(&startOk);
            qint64 end = m_payload.size() - 1;
            if (!match.captured(2).isEmpty()) {
                bool endOk = false;
                end = match.captured(2).toLongLong(&endOk);
                if (!endOk) {
                    respond(socket, 416, {}, {});
                    return;
                }
            }

            if (!startOk || start < 0 || start >= m_payload.size() || end < start) {
                respond(socket, 416,
                        {{"Content-Range",
                          QByteArray("bytes */") + QByteArray::number(m_payload.size())}},
                        {});
                return;
            }
            end = qMin<qint64>(end, m_payload.size() - 1);
            const QByteArray body = m_payload.mid(start, end - start + 1);
            const QByteArray contentRange =
                QByteArrayLiteral("bytes ") + QByteArray::number(start) + '-'
                + QByteArray::number(end) + '/' + QByteArray::number(m_payload.size());
            respond(socket, 206,
                    {{"Content-Range", contentRange},
                     {"Content-Length", QByteArray::number(body.size())}},
                    body);
            return;
        }

        respond(socket, 404, {}, {});
    }

    void respondJson(QTcpSocket *socket, const QJsonObject &object)
    {
        respond(socket, 200,
                {{"Content-Type", "application/json; charset=utf-8"}},
                QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

    void respond(QTcpSocket *socket, int status,
                 const QList<QPair<QByteArray, QByteArray>> &headers,
                 const QByteArray &body)
    {
        static const QHash<int, QByteArray> reasons{
            {200, "OK"},
            {206, "Partial Content"},
            {400, "Bad Request"},
            {404, "Not Found"},
            {416, "Range Not Satisfiable"},
        };
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' '
            + reasons.value(status, "Error") + "\r\n";
        bool hasLength = false;
        for (const auto &header : headers) {
            response += header.first + ": " + header.second + "\r\n";
            if (header.first.compare("Content-Length", Qt::CaseInsensitive) == 0) {
                hasLength = true;
            }
        }
        if (!hasLength) {
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        }
        response += "Connection: close\r\n\r\n";
        response += body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QByteArray m_payload;
    QList<QByteArray> m_ranges;
    int m_downloadUrlCalls = 0;
};

struct RangeResult
{
    QByteArray data;
    qint64 size = 0;
    int error = 0;
    bool opened = false;
};

QUrl streamingUrl()
{
    return QUrl(QStringLiteral("chomikujstream://user/movie.mkv"));
}

RangeResult readRange(qint64 offset, qint64 length)
{
    RangeResult result;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    KIO::FileJob *job = KIO::open(streamingUrl(), QIODevice::ReadOnly);
    QObject::connect(job, &KIO::FileJob::open, &loop, [&](KIO::Job *) {
        result.opened = true;
        result.size = job->size();
        job->seek(offset);
    });
    QObject::connect(job, &KIO::FileJob::position, &loop,
                     [&](KIO::Job *, KIO::filesize_t position) {
        if (position != static_cast<KIO::filesize_t>(offset)) {
            job->close();
            return;
        }
        job->read(length);
    });
    QObject::connect(job, &KIO::FileJob::data, &loop,
                     [&](KIO::Job *, const QByteArray &chunk) {
        result.data += chunk;
        if (result.data.size() >= length) {
            job->close();
        }
    });
    QObject::connect(job, &KIO::FileJob::fileClosed, &loop, [&](KIO::Job *) {
        loop.quit();
    });
    QObject::connect(job, &KIO::FileJob::result, &loop, [&](KJob *finished) {
        if (finished->error()) {
            result.error = finished->error();
            loop.quit();
        }
    });

    timer.start(s_timeoutMs);
    loop.exec();
    if (job->error() && result.error == 0) {
        result.error = job->error();
    }
    job->deleteLater();
    return result;
}

} // namespace

class KioOpenTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void randomAccess();
    void readAtEof();
    void writeOpenFails();
    void statAdvertisesStreamingTarget();
    void listAdvertisesStreamingTarget();
    void publicAccountUsesDefaultReadOnlyRouting();
    void fullGetStillWorks();
    void getResume();

private:
    MockServer *m_mock = nullptr;
};

void KioOpenTest::initTestCase()
{
    m_mock = new MockServer(this);
    QVERIFY2(m_mock->isValid(), "mock server failed to listen");

    qputenv("CHOMIKUJ_BASE_URL",
            QStringLiteral("http://127.0.0.1:%1").arg(m_mock->port()).toUtf8());
    qputenv("CHOMIKUJ_ACCOUNT", "user");
    qputenv("CHOMIKUJ_PASSWORD", "pass");
    qputenv("CHOMIKUJ_RETRY_BASE_MS", "1");
    qputenv("CHOMIKUJ_RETRY_MAX_MS", "1");
    qputenv("CHOMIKUJ_STREAMING_TARGET_URL", "all");

    QVERIFY(KProtocolManager::supportsOpening(streamingUrl()));
    QVERIFY(KProtocolManager::supportsTruncating(streamingUrl()));
}

void KioOpenTest::randomAccess()
{
    const QByteArray content = m_mock->content();

    const QList<QPair<qint64, qint64>> ranges{
        {0, 4096},
        {1, 16},
        {50000, 8192},
        {content.size() - 5, 5},
    };

    for (const auto &range : ranges) {
        const RangeResult result = readRange(range.first, range.second);
        QVERIFY2(result.opened, "FileJob did not open");
        QCOMPARE(result.error, 0);
        QCOMPARE(result.size, qint64(content.size()));
        const qint64 wanted = qMin(range.second, qint64(content.size()) - range.first);
        QCOMPARE(result.data, content.mid(range.first, wanted));
    }

    QVERIFY(m_mock->downloadUrlCalls() >= 1);
    QVERIFY(!m_mock->ranges().isEmpty());
}

void KioOpenTest::readAtEof()
{
    const QByteArray content = m_mock->content();
    const RangeResult result = readRange(content.size(), 0);
    QVERIFY(result.opened);
    QCOMPARE(result.error, 0);
    QCOMPARE(result.size, qint64(content.size()));
    QVERIFY(result.data.isEmpty());
}

void KioOpenTest::writeOpenFails()
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    int error = 0;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    KIO::FileJob *job = KIO::open(streamingUrl(), QIODevice::ReadWrite);
    QObject::connect(job, &KIO::FileJob::result, &loop, [&](KJob *finished) {
        error = finished->error();
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY(error != 0);
    job->deleteLater();
}

void KioOpenTest::statAdvertisesStreamingTarget()
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QString target;
    KIO::StatJob *job = KIO::stat(QUrl(QStringLiteral("chomikuj://user/movie.mkv")));
    job->setSide(KIO::StatJob::SourceSide);
    QObject::connect(job, &KIO::StatJob::result, &loop, [&](KJob *finished) {
        if (!finished->error()) {
            target = job->statResult().stringValue(KIO::UDSEntry::UDS_TARGET_URL);
        }
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY2(job->error() == 0, qPrintable(job->errorString()));
    QCOMPARE(QUrl(target).scheme(), QStringLiteral("chomikujstream"));
    QCOMPARE(QUrl(target).path(), QStringLiteral("/movie.mkv"));
    job->deleteLater();
}

void KioOpenTest::listAdvertisesStreamingTarget()
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    bool sawVideoTarget = false;
    bool sawTextTarget = false;
    KIO::ListJob *job = KIO::listDir(QUrl(QStringLiteral("chomikuj://user/")));
    QObject::connect(job, &KIO::ListJob::entries, &loop,
                     [&](KIO::Job *, const KIO::UDSEntryList &entries) {
        for (const KIO::UDSEntry &entry : entries) {
            const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
            const QString target = entry.stringValue(KIO::UDSEntry::UDS_TARGET_URL);
            if (name == QLatin1String("movie.mkv")) {
                sawVideoTarget = QUrl(target).scheme() == QLatin1String("chomikujstream");
            }
            if (name == QLatin1String("notes.txt")) {
                sawTextTarget = !target.isEmpty();
            }
        }
    });
    QObject::connect(job, &KIO::ListJob::result, &loop, [&](KJob *) {
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY2(job->error() == 0, qPrintable(job->errorString()));
    QVERIFY(sawVideoTarget);
    QVERIFY(!sawTextTarget);
    job->deleteLater();
}

void KioOpenTest::publicAccountUsesDefaultReadOnlyRouting()
{
    qunsetenv("CHOMIKUJ_STREAMING_TARGET_URL");

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QString target;
    KIO::StatJob *job = KIO::stat(QUrl(QStringLiteral("chomikuj://other/movie.mkv")));
    job->setSide(KIO::StatJob::SourceSide);
    QObject::connect(job, &KIO::StatJob::result, &loop, [&](KJob *finished) {
        if (!finished->error()) {
            target = job->statResult().stringValue(KIO::UDSEntry::UDS_TARGET_URL);
        }
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY2(job->error() == 0, qPrintable(job->errorString()));
    QCOMPARE(QUrl(target).scheme(), QStringLiteral("chomikujstream"));
    QCOMPARE(QUrl(target).host(), QStringLiteral("other"));
    QCOMPARE(QUrl(target).path(), QStringLiteral("/movie.mkv"));
    job->deleteLater();

    qputenv("CHOMIKUJ_STREAMING_TARGET_URL", "all");
}

void KioOpenTest::fullGetStillWorks()
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    QByteArray data;
    KIO::TransferJob *job = KIO::get(streamingUrl(), KIO::NoReload,
                                     KIO::HideProgressInfo);
    QObject::connect(job, &KIO::TransferJob::data, &loop,
                     [&](KIO::Job *, const QByteArray &chunk) {
        data += chunk;
    });
    QObject::connect(job, &KIO::TransferJob::result, &loop, [&](KJob *) {
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY2(job->error() == 0, qPrintable(job->errorString()));
    QCOMPARE(data, m_mock->content());
    job->deleteLater();
}

void KioOpenTest::getResume()
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    constexpr qint64 offset = 1000;
    QByteArray data;
    KIO::TransferJob *job = KIO::get(streamingUrl(), KIO::NoReload,
                                     KIO::HideProgressInfo);
    job->addMetaData(QStringLiteral("range-start"), QString::number(offset));
    QObject::connect(job, &KIO::TransferJob::data, &loop,
                     [&](KIO::Job *, const QByteArray &chunk) {
        data += chunk;
    });
    QObject::connect(job, &KIO::TransferJob::result, &loop, [&](KJob *) {
        loop.quit();
    });
    timer.start(s_timeoutMs);
    loop.exec();
    QVERIFY2(job->error() == 0, qPrintable(job->errorString()));
    QCOMPARE(data, m_mock->content().mid(offset));
    QVERIFY(m_mock->ranges().contains(QByteArray("bytes=1000-")));
    job->deleteLater();
}

QTEST_GUILESS_MAIN(KioOpenTest)

#include "kio_open_check.moc"

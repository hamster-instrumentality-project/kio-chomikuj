/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/chomikujtypes.h"
#include "core/signing.h"
#include "core/uploader.h"

#include <QFile>
#include <QTemporaryFile>
#include <QtTest>

class SigningTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void secret();
    void loginVector();
    void queryVector();
    void queryEncoding();
    void names();
    void crc32();
    void escaping();
};

void SigningTest::secret()
{
    QCOMPARE(Chomikuj::decodedSecret(), QStringLiteral("wzrwYua$.DSe8suk!`'2"));
}

void SigningTest::loginVector()
{
    const QByteArray body = "{\"AccountName\":\"a\",\"Password\":\"b\"}";
    QCOMPARE(Chomikuj::sign(QStringLiteral("/api/v3/account/login"), QString(), body),
             QStringLiteral("aad620459d9baab0e5541d1159fdb0a8"));
}

void SigningTest::queryVector()
{
    QCOMPARE(Chomikuj::sign(QStringLiteral("/api/v3/folders"),
                            QStringLiteral("Parent=&page=1"), QByteArray()),
             QStringLiteral("e4b2d8378214d450545ffea0e71c9e3f"));
}

void SigningTest::queryEncoding()
{
    const QList<QPair<QString, QString>> params = {
        {QStringLiteral("Query"), QStringLiteral("hello world")},
        {QStringLiteral("PageNumber"), QStringLiteral("1")},
    };
    QCOMPARE(Chomikuj::buildQuery(params),
             QStringLiteral("Query=hello%20world&PageNumber=1"));
    QCOMPARE(Chomikuj::encodeQueryComponent(QStringLiteral("a/b")), QStringLiteral("a%2Fb"));
    QCOMPARE(Chomikuj::encodeQueryComponent(QStringLiteral("zażółć")),
             QStringLiteral("za%C5%BC%C3%B3%C5%82%C4%87"));
}

void SigningTest::names()
{
    QCOMPARE(Chomikuj::joinName(QStringLiteral("a.b.c"), QStringLiteral("bin")),
             QStringLiteral("a.b.c.bin"));
    QCOMPARE(Chomikuj::joinName(QStringLiteral("movie.mkv"), QStringLiteral("mkv")),
             QStringLiteral("movie.mkv"));
    QCOMPARE(Chomikuj::joinName(QStringLiteral("README"), QString()), QStringLiteral("README"));

    QString base;
    QString extension;
    Chomikuj::splitName(QStringLiteral("a.b.c.bin"), &base, &extension);
    QCOMPARE(base, QStringLiteral("a.b.c"));
    QCOMPARE(extension, QStringLiteral("bin"));
}

void SigningTest::crc32()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("123456789"), qint64(9));
    file.flush();
    QCOMPARE(Chomikuj::crc32Hex(file.fileName()), QStringLiteral("cbf43926"));

    QTemporaryFile empty;
    QVERIFY(empty.open());
    empty.flush();
    QCOMPARE(Chomikuj::crc32Hex(empty.fileName()), QStringLiteral("0"));
}

void SigningTest::escaping()
{
    QCOMPARE(Chomikuj::escapeFilename(QStringLiteral("a?b:c<d>e/f*g\"h\\i")),
             QStringLiteral("a_b_c_d_e_f_g_h_i"));
    QCOMPARE(Chomikuj::escapeFilename(QStringLiteral("normal_file.mp3")),
             QStringLiteral("normal_file.mp3"));
}

QTEST_GUILESS_MAIN(SigningTest)

#include "tst_signing.moc"

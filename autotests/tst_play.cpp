/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "play/playurl.h"

#include <QtTest>

class PlayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rewritesChomikujUrl();
    void keepsStreamingUrl();
    void leavesOtherUrlsAlone();
    void preservesPathAndQuery();
};

void PlayTest::rewritesChomikujUrl()
{
    const QUrl url(QStringLiteral("chomikuj://account/Movies/film.mkv"));
    const QUrl result = Chomikuj::streamingUrl(url);

    QCOMPARE(result.scheme(), QStringLiteral("chomikujstream"));
    QCOMPARE(result.host(), QStringLiteral("account"));
    QCOMPARE(result.path(), QStringLiteral("/Movies/film.mkv"));
    QVERIFY(Chomikuj::isChomikujUrl(url));
}

void PlayTest::keepsStreamingUrl()
{
    const QUrl url(QStringLiteral("chomikujstream://account/Movies/film.mkv"));
    QCOMPARE(Chomikuj::streamingUrl(url), url);
    QVERIFY(Chomikuj::isChomikujUrl(url));
}

void PlayTest::leavesOtherUrlsAlone()
{
    const QUrl local = QUrl::fromLocalFile(QStringLiteral("/tmp/film.mkv"));
    const QUrl http(QStringLiteral("https://example.test/film.mkv"));

    QCOMPARE(Chomikuj::streamingUrl(local), local);
    QCOMPARE(Chomikuj::streamingUrl(http), http);
    QVERIFY(!Chomikuj::isChomikujUrl(local));
    QVERIFY(!Chomikuj::isChomikujUrl(http));
}

void PlayTest::preservesPathAndQuery()
{
    const QUrl url(QStringLiteral(
        "chomikuj://account/Movies/a%20b.mkv?download=1#chapter"));
    const QUrl result = Chomikuj::streamingUrl(url);

    QCOMPARE(result.scheme(), QStringLiteral("chomikujstream"));
    QCOMPARE(result.path(), QStringLiteral("/Movies/a b.mkv"));
    QCOMPARE(result.query(), QStringLiteral("download=1"));
    QCOMPARE(result.fragment(), QStringLiteral("chapter"));
}

QTEST_GUILESS_MAIN(PlayTest)

#include "tst_play.moc"

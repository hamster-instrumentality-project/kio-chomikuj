/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KProtocolInfo>
#include <KProtocolManager>

#include <QtTest>

class PluginTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void protocolIsRegistered();
    void streamingProtocolIsRegistered();
};

void PluginTest::protocolIsRegistered()
{
    QVERIFY(KProtocolInfo::isKnownProtocol(QStringLiteral("chomikuj")));
    QCOMPARE(KProtocolInfo::protocolClass(QStringLiteral("chomikuj")), QStringLiteral(":internet"));
    QCOMPARE(KProtocolInfo::icon(QStringLiteral("chomikuj")), QStringLiteral("folder-remote"));
    QCOMPARE(KProtocolInfo::docPath(QStringLiteral("chomikuj")),
             QStringLiteral("kioworker6/chomikuj/index.html"));
    QVERIFY(KProtocolManager::canCopyFromFile(QUrl(QStringLiteral("chomikuj://account/"))));
    QVERIFY(KProtocolInfo::maxWorkers(QStringLiteral("chomikuj")) > 1);
    QVERIFY(KProtocolInfo::maxWorkersPerHost(QStringLiteral("chomikuj")) > 1);

    // The normal protocol deliberately keeps the kio-fuse cache-based path so
    // writes through a kio-fuse mount continue to work.
    QVERIFY(!KProtocolManager::supportsOpening(QUrl(QStringLiteral("chomikuj://account/file"))));
    QVERIFY(!KProtocolManager::supportsTruncating(QUrl(QStringLiteral("chomikuj://account/file"))));
}

void PluginTest::streamingProtocolIsRegistered()
{
    QVERIFY(KProtocolInfo::isKnownProtocol(QStringLiteral("chomikujstream")));
    QCOMPARE(KProtocolInfo::protocolClass(QStringLiteral("chomikujstream")),
             QStringLiteral(":internet"));
    QCOMPARE(KProtocolInfo::icon(QStringLiteral("chomikujstream")),
             QStringLiteral("folder-remote"));
    QVERIFY(KProtocolManager::supportsOpening(
        QUrl(QStringLiteral("chomikujstream://account/file"))));
    // kio-fuse 5.1 requires both flags before choosing its FileJob path.
    QVERIFY(KProtocolManager::supportsTruncating(
        QUrl(QStringLiteral("chomikujstream://account/file"))));
    QVERIFY(!KProtocolManager::canCopyFromFile(
        QUrl(QStringLiteral("chomikujstream://account/"))));
}

QTEST_GUILESS_MAIN(PluginTest)

#include "tst_plugin.moc"

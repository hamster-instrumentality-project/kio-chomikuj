/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <KProtocolInfo>

#include <QtTest>

class PluginTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void protocolIsRegistered();
};

void PluginTest::protocolIsRegistered()
{
    QVERIFY(KProtocolInfo::isKnownProtocol(QStringLiteral("chomikuj")));
    QCOMPARE(KProtocolInfo::protocolClass(QStringLiteral("chomikuj")), QStringLiteral(":internet"));
    QCOMPARE(KProtocolInfo::icon(QStringLiteral("chomikuj")), QStringLiteral("folder-remote"));
}

QTEST_GUILESS_MAIN(PluginTest)

#include "tst_plugin.moc"

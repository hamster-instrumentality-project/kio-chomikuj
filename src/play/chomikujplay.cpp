/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * One-shot launcher for Chomikuj videos.  It rewrites chomikuj:// URLs to the
 * read-only chomikujstream:// protocol, asks kio-fuse for the corresponding
 * local path, and starts the configured media player.
 *
 * This is deliberately not a daemon: the KIO worker owns the protocol and
 * session; this helper only bridges the Dolphin/open-with flow to kio-fuse.
 */

#include "playurl.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <KConfigGroup>
#include <KSharedConfig>

#include <cstdio>

namespace
{

int showError(const QString &message)
{
    QTextStream(stderr) << "kio-chomikuj-play: " << message << '\n';
    if (QApplication::instance()) {
        QMessageBox::critical(nullptr, QStringLiteral("Chomikuj"), message);
    }
    return 1;
}

QString configuredPlayer()
{
    const QString environment = qEnvironmentVariable("CHOMIKUJ_STREAMING_PLAYER");
    if (!environment.isEmpty()) {
        return environment;
    }

    const KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("chomikujrc")),
                             QStringLiteral("Streaming"));
    const QString player = group.readEntry(QStringLiteral("Player"), QStringLiteral("mpv")).trimmed();
    return player.isEmpty() ? QStringLiteral("mpv") : player;
}

QString mountPath(const QUrl &url, QString *errorMessage)
{
    if (!QDBusConnection::sessionBus().isConnected()) {
        *errorMessage = QStringLiteral(
            "No D-Bus session is available; kio-fuse cannot mount Chomikuj URLs.");
        return {};
    }

    // Do not check QDBusInterface::isValid() here: the kio-fuse service is
    // D-Bus-activated, so validity may only become true after the call.
    QDBusInterface interface(QStringLiteral("org.kde.KIOFuse"),
                             QStringLiteral("/org/kde/KIOFuse"),
                             QStringLiteral("org.kde.KIOFuse.VFS"),
                             QDBusConnection::sessionBus());
    const QDBusReply<QString> reply =
        interface.call(QStringLiteral("mountUrl"), url.toString());
    if (!reply.isValid() || reply.value().isEmpty()) {
        *errorMessage = reply.isValid()
            ? QStringLiteral("kio-fuse did not return a local path for %1").arg(url.toString())
            : (reply.error().message().isEmpty()
                   ? QStringLiteral("kio-fuse is not available")
                   : reply.error().message());
        return {};
    }
    return reply.value();
}

QString resolvePlayer(const QString &player)
{
    if (player.contains(QLatin1Char('/'))) {
        return QFileInfo(player).isExecutable() ? player : QString();
    }
    return QStandardPaths::findExecutable(player);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kio-chomikuj-play"));
    QApplication::setApplicationDisplayName(QStringLiteral("Chomikuj streaming player"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Open a Chomikuj video through the read-only streaming protocol"));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("url"),
                                 QStringLiteral("chomikuj:// URL or local file to play"));

    QCommandLineOption playerOption(
        QStringLiteral("player"),
        QStringLiteral("Media player executable (default: the [Streaming] Player setting or mpv)"),
        QStringLiteral("command"));
    QCommandLineOption printUrlOption(
        QStringLiteral("print-url"),
        QStringLiteral("Print the rewritten streaming URL and exit"));
    QCommandLineOption printPathOption(
        QStringLiteral("print-path"),
        QStringLiteral("Print the kio-fuse path and exit without launching a player"));
    parser.addOption(playerOption);
    parser.addOption(printUrlOption);
    parser.addOption(printPathOption);
    parser.process(app);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.size() != 1) {
        return showError(QStringLiteral("expected exactly one URL"));
    }

    QUrl url = QUrl::fromUserInput(arguments.first());
    if (!url.isValid()) {
        return showError(QStringLiteral("invalid URL: %1").arg(arguments.first()));
    }

    const bool localFile = url.isLocalFile();
    if (!localFile && !Chomikuj::isChomikujUrl(url)) {
        return showError(QStringLiteral("not a Chomikuj URL: %1").arg(url.toString()));
    }

    if (parser.isSet(printUrlOption)) {
        QTextStream(stdout) << Chomikuj::streamingUrl(url).toString(QUrl::FullyEncoded) << '\n';
        return 0;
    }

    QString path;
    if (localFile) {
        path = url.toLocalFile();
    } else {
        QString errorMessage;
        path = mountPath(Chomikuj::streamingUrl(url), &errorMessage);
        if (path.isEmpty()) {
            return showError(errorMessage);
        }
    }

    if (parser.isSet(printPathOption)) {
        QTextStream(stdout) << path << '\n';
        return 0;
    }

    const QString player = parser.isSet(playerOption)
        ? parser.value(playerOption)
        : configuredPlayer();
    const QString playerPath = resolvePlayer(player);
    if (playerPath.isEmpty()) {
        return showError(QStringLiteral("media player not found: %1").arg(player));
    }

    if (!QProcess::startDetached(playerPath, {path})) {
        return showError(QStringLiteral("could not start %1").arg(playerPath));
    }
    return 0;
}

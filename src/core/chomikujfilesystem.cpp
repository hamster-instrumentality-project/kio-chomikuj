/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "chomikujfilesystem.h"

#include "chomikujclient.h"
#include "chomikujerror.h"

#include <QStringList>

#include <KIO/Global>

#include <optional>

namespace Chomikuj
{

namespace
{

constexpr qint64 s_rootId = 0;

QStringList pathSegments(const QString &path)
{
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));

    QStringList result;
    const QStringList raw = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : raw) {
        if (segment == QLatin1String(".")) {
            continue;
        }
        if (segment == QLatin1String("..")) {
            if (!result.isEmpty()) {
                result.removeLast();
            }
            continue;
        }
        result.append(segment);
    }
    return result;
}

QString normalizedPath(const QString &path)
{
    const QStringList segments = pathSegments(path);
    if (segments.isEmpty()) {
        return QStringLiteral("/");
    }
    return QLatin1Char('/') + segments.join(QLatin1Char('/'));
}

std::optional<Entry> findChildOptional(const QList<Entry> &entries, const QString &name)
{
    for (const Entry &entry : entries) {
        if (entry.name == name) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace

ChomikujFileSystem::ChomikujFileSystem(ChomikujClient &client)
    : m_client(client)
{
}

QList<Entry> ChomikujFileSystem::list(const QString &path)
{
    const qint64 folderId = resolveFolder(path);
    return listFolder(folderId);
}

Entry ChomikujFileSystem::stat(const QString &path)
{
    return resolveEntry(path);
}

Entry ChomikujFileSystem::mkdir(const QString &path)
{
    qint64 parentId = 0;
    QString name;
    resolveParent(path, &parentId, &name);

    const QList<Entry> siblings = listFolder(parentId, false);
    if (findChildOptional(siblings, name).has_value()) {
        throw Error(KIO::ERR_DIR_ALREADY_EXIST,
                    QStringLiteral("“%1” already exists").arg(name));
    }

    const qint64 id = m_client.createFolder(name, parentId);
    clearCache();

    Entry entry;
    entry.id = id;
    entry.name = name;
    entry.isDir = true;
    entry.accountId = m_client.accountId().toLongLong();
    return entry;
}

void ChomikujFileSystem::rename(const QString &source, const QString &destination, bool overwrite)
{
    const QString sourcePath = normalizedPath(source);
    const QString destinationPath = normalizedPath(destination);

    qint64 sourceParent = 0;
    qint64 destinationParent = 0;
    QString sourceName;
    QString destinationName;
    resolveParent(sourcePath, &sourceParent, &sourceName);
    resolveParent(destinationPath, &destinationParent, &destinationName);

    const Entry sourceEntry = findChild(sourceParent, sourceName, false);
    if (sourceEntry.isDir && destinationPath.startsWith(sourcePath + QLatin1Char('/'))) {
        throw Error(KIO::ERR_WORKER_DEFINED,
                    QStringLiteral("A folder cannot be moved into itself"));
    }

    if (sourceParent == destinationParent && sourceName == destinationName) {
        return;
    }

    const std::optional<Entry> existing =
        findChildOptional(listFolder(destinationParent, false), destinationName);
    if (existing.has_value() && existing->id != sourceEntry.id) {
        if (existing->isDir) {
            throw Error(KIO::ERR_DIR_ALREADY_EXIST,
                        QStringLiteral("“%1” already exists").arg(destinationName));
        }
        if (!overwrite) {
            throw Error(KIO::ERR_FILE_ALREADY_EXIST,
                        QStringLiteral("“%1” already exists").arg(destinationName));
        }
        m_client.deleteItems({existing->id}, {});
        invalidate(destinationParent);
    }

    if (sourceParent != destinationParent) {
        if (sourceEntry.isDir) {
            m_client.moveItems(sourceParent, destinationParent, {}, {sourceEntry.id});
        } else {
            m_client.moveItems(sourceParent, destinationParent, {sourceEntry.id}, {});
        }
    }

    if (sourceName != destinationName) {
        if (sourceEntry.isDir) {
            m_client.changeFolderName(sourceEntry.id, destinationName);
        } else {
            m_client.changeFileName(sourceEntry.id, destinationName);
        }
    }

    clearCache();
}

void ChomikujFileSystem::remove(const QString &path, bool isFile, bool recursive)
{
    qint64 parentId = 0;
    QString name;
    resolveParent(path, &parentId, &name);

    const Entry entry = findChild(parentId, name, false);
    Q_UNUSED(isFile)

    if (entry.isDir) {
        if (!recursive && !listFolder(entry.id, false).isEmpty()) {
            throw Error(KIO::ERR_CANNOT_RMDIR,
                        QStringLiteral("The folder “%1” is not empty").arg(name));
        }
        m_client.deleteItems({}, {entry.id});
    } else {
        m_client.deleteItems({entry.id}, {});
    }

    clearCache();
}

void ChomikujFileSystem::upload(const QString &localPath, const QString &destination,
                                bool overwrite,
                                const ProgressCallback &progress,
                                const std::function<bool()> &cancelled)
{
    qint64 parentId = 0;
    QString name;
    resolveParent(destination, &parentId, &name);

    const std::optional<Entry> existing =
        findChildOptional(listFolder(parentId, false), name);
    if (existing.has_value()) {
        if (existing->isDir) {
            throw Error(KIO::ERR_DIR_ALREADY_EXIST,
                        QStringLiteral("“%1” is a folder").arg(name));
        }
        if (!overwrite) {
            throw Error(KIO::ERR_FILE_ALREADY_EXIST,
                        QStringLiteral("“%1” already exists").arg(name));
        }
        m_client.deleteItems({existing->id}, {});
        invalidate(parentId);
    }

    m_client.uploadFile(localPath, parentId, name, progress, cancelled);
    clearCache();
}

void ChomikujFileSystem::download(const QString &path,
                                  const DataCallback &onData,
                                  const ProgressCallback &onProgress,
                                  const std::function<bool()> &cancelled)
{
    const Entry entry = resolveEntry(path);
    if (entry.isDir) {
        throw Error(KIO::ERR_IS_DIRECTORY,
                    QStringLiteral("“%1” is a folder").arg(entry.name));
    }
    m_client.downloadFile(entry.id, onData, onProgress, cancelled);
}

void ChomikujFileSystem::clearCache()
{
    m_cache.clear();
}

QList<Entry> ChomikujFileSystem::listFolder(qint64 folderId, bool useCache)
{
    if (useCache) {
        const auto cached = m_cache.constFind(folderId);
        if (cached != m_cache.cend()) {
            return *cached;
        }
    }

    const QList<Entry> entries = m_client.listFolder(folderId);
    m_cache.insert(folderId, entries);
    return entries;
}

Entry ChomikujFileSystem::findChild(qint64 parentId, const QString &name, bool useCache)
{
    const std::optional<Entry> entry = findChildOptional(listFolder(parentId, useCache), name);
    if (!entry.has_value()) {
        throw Error(KIO::ERR_DOES_NOT_EXIST,
                    QStringLiteral("“%1” was not found").arg(name));
    }
    return *entry;
}

qint64 ChomikujFileSystem::resolveFolder(const QString &path)
{
    const QStringList segments = pathSegments(path);
    qint64 current = s_rootId;

    for (const QString &segment : segments) {
        const Entry entry = findChild(current, segment);
        if (!entry.isDir) {
            throw Error(KIO::ERR_CANNOT_ENTER_DIRECTORY,
                        QStringLiteral("“%1” is not a folder").arg(segment));
        }
        current = entry.id;
    }
    return current;
}

Entry ChomikujFileSystem::resolveEntry(const QString &path)
{
    const QStringList segments = pathSegments(path);
    if (segments.isEmpty()) {
        Entry root;
        root.id = s_rootId;
        root.name = QStringLiteral("/");
        root.isDir = true;
        return root;
    }

    qint64 current = s_rootId;
    Entry result;
    for (int i = 0; i < segments.size(); ++i) {
        result = findChild(current, segments.at(i));
        if (i + 1 < segments.size()) {
            if (!result.isDir) {
                throw Error(KIO::ERR_CANNOT_ENTER_DIRECTORY,
                            QStringLiteral("“%1” is not a folder").arg(result.name));
            }
            current = result.id;
        }
    }
    return result;
}

void ChomikujFileSystem::resolveParent(const QString &path, qint64 *parentId, QString *name)
{
    const QStringList segments = pathSegments(path);
    if (segments.isEmpty()) {
        throw Error(KIO::ERR_WORKER_DEFINED,
                    QStringLiteral("A file or folder name is required"));
    }

    *name = segments.last();
    QString parentPath = QStringLiteral("/");
    if (segments.size() > 1) {
        parentPath = QLatin1Char('/') + QStringList(segments.mid(0, segments.size() - 1)).join(QLatin1Char('/'));
    }
    *parentId = resolveFolder(parentPath);
}

void ChomikujFileSystem::invalidate(qint64 folderId)
{
    m_cache.remove(folderId);
}

} // namespace Chomikuj

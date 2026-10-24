/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "chomikujtypes.h"
#include "httptransport.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>

namespace Chomikuj
{

class ChomikujClient;

class ChomikujFileSystem
{
public:
    explicit ChomikujFileSystem(ChomikujClient &client);

    QList<Entry> list(const QString &path);
    Entry stat(const QString &path);
    Entry mkdir(const QString &path);

    void rename(const QString &source, const QString &destination, bool overwrite);
    void remove(const QString &path, bool isFile, bool recursive);

    void upload(const QString &localPath, const QString &destination,
                bool overwrite,
                const ProgressCallback &progress,
                const std::function<bool()> &cancelled);

    void download(const QString &path,
                  const DataCallback &onData,
                  const ProgressCallback &onProgress,
                  const std::function<bool()> &cancelled);

    void clearCache();

private:
    QList<Entry> listFolder(qint64 folderId, bool useCache = true);
    Entry findChild(qint64 parentId, const QString &name, bool useCache = true);
    qint64 resolveFolder(const QString &path);
    Entry resolveEntry(const QString &path);
    void resolveParent(const QString &path, qint64 *parentId, QString *name);
    void invalidate(qint64 folderId);

    ChomikujClient &m_client;
    QHash<qint64, QList<Entry>> m_cache;
};

} // namespace Chomikuj

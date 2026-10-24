/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

namespace Chomikuj
{

struct Folder
{
    qint64 id = 0;
    QString name;
    bool hasPassword = false;
    bool isAdult = false;
    bool isHidden = false;
    bool isPublicUploadEnabled = false;
    qint64 accountId = 0;
};

struct File
{
    qint64 id = 0;
    QString name;       // base name without extension
    QString extension;  // extension without the dot, may be empty
    qint64 sizeBytes = 0; // server reports floored kilobytes, converted here
    QString mediaType;
    bool isHidden = false;
    qint64 accountId = 0;

    QString fullName() const;
};

struct Entry
{
    qint64 id = 0;
    QString name;       // full, user-visible name (extension included)
    QString extension;  // file extension without the dot, empty for folders
    qint64 sizeBytes = 0; // 0 for folders; floored KB * 1024 for files
    QString mediaType;
    bool isDir = false;
    bool isHidden = false;
    bool hasPassword = false;
    qint64 accountId = 0;
};

struct Listing
{
    qint64 parentId = 0;
    QString parentName;
    QList<Folder> folders;
    QList<File> files;
    bool nextPageAvailable = false;
};

struct AccountSearchItem
{
    qint64 id = 0;
    QString name;
};

struct LoginResult
{
    QString accountId;
    QString accountName;
    QString apiKey;
    QString avatarUrl;
};

struct UploadSlot
{
    QString url;
    qint64 chunk = 0;
    bool completed = false;
};

// Splits a name at the last dot, as the server does on upload.
void splitName(const QString &fullName, QString *base, QString *extension);

// Joins base and extension, avoiding a duplicated extension.  The rename
// endpoint stores the full name including the dot, so "movie.mkv" + "mkv"
// must stay "movie.mkv".
QString joinName(const QString &base, const QString &extension);

QString fileExtension(const QString &name);
QString mimeTypeForName(const QString &name);

} // namespace Chomikuj

/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "httptransport.h"

#include <QString>

#include <functional>

namespace Chomikuj
{

class ChomikujClient;

// CRC32 of a file as a lowercase, unpadded hex string.
QString crc32Hex(const QString &path);

// Mirrors the Android client's filename escaping for multipart uploads.
QString escapeFilename(const QString &name);

void uploadFile(ChomikujClient &client,
                const QString &localPath,
                qint64 folderId,
                const QString &remoteName,
                const ProgressCallback &progress,
                const std::function<bool()> &cancelled);

} // namespace Chomikuj

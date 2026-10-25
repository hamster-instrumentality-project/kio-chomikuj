/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QUrl>

namespace Chomikuj
{

// Rewrites a normal chomikuj:// URL to the read-only streaming protocol.
// Other URLs (including an already-rewritten chomikujstream:// URL) are
// returned unchanged.
QUrl streamingUrl(const QUrl &url);

bool isChomikujUrl(const QUrl &url);

} // namespace Chomikuj

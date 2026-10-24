/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <functional>

namespace Chomikuj
{

// Exponential backoff with full jitter, matching the hardened gvfs client.
// The base and ceiling can be overridden with CHOMIKUJ_RETRY_BASE_MS and
// CHOMIKUJ_RETRY_MAX_MS.
int retryDelayMs(int attempt);

// Sleeps for the requested duration while remaining responsive to cancellation.
void sleepInterruptible(int milliseconds, const std::function<bool()> &cancelled);

} // namespace Chomikuj

/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "retry.h"

#include <QRandomGenerator>
#include <QThread>

namespace Chomikuj
{

namespace
{

constexpr int s_defaultBaseMs = 500;
constexpr int s_defaultMaxMs = 8000;

int environmentMs(const char *name, int fallback)
{
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok && value > 0 ? value : fallback;
}

} // namespace

int retryDelayMs(int attempt)
{
    const int base = environmentMs("CHOMIKUJ_RETRY_BASE_MS", s_defaultBaseMs);
    const int maximum = qMax(base, environmentMs("CHOMIKUJ_RETRY_MAX_MS", s_defaultMaxMs));
    const int shift = qBound(0, attempt, 6);
    const qint64 ceiling = qMin<qint64>(maximum, static_cast<qint64>(base) << shift);

    // Full jitter: spread retries over the whole exponential window instead of
    // hammering a restarting or rate-limited server at fixed intervals.
    if (ceiling <= 1) {
        return static_cast<int>(ceiling);
    }
    return QRandomGenerator::global()->bounded(static_cast<int>(ceiling));
}

void sleepInterruptible(int milliseconds, const std::function<bool()> &cancelled)
{
    while (milliseconds > 0) {
        if (cancelled && cancelled()) {
            return;
        }
        const int step = qMin(milliseconds, 50);
        QThread::msleep(static_cast<unsigned long>(step));
        milliseconds -= step;
    }
}

} // namespace Chomikuj

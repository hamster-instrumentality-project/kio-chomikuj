/*
 * SPDX-FileCopyrightText: 2026 kio-chomikuj contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "uploader.h"

#include "chomikujclient.h"
#include "chomikujerror.h"
#include "retry.h"

#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <KIO/Global>

#include <zlib.h>

namespace Chomikuj
{

namespace
{

constexpr int s_maxReconcileAttempts = 5;
constexpr int s_maxReconcilePolls = 5;
constexpr int s_maxTerminalResponses = 2;
constexpr int s_chunkTimeoutMs = 30000;

bool isCancelled(const std::function<bool()> &cancelled)
{
    return cancelled && cancelled();
}

void throwIfCancelled(const std::function<bool()> &cancelled)
{
    if (isCancelled(cancelled)) {
        throw Error(KIO::ERR_USER_CANCELED, QStringLiteral("Upload cancelled"));
    }
}

qint64 expectedStoredSize(qint64 size)
{
    return (size / 1024) * 1024;
}

QByteArray boundary()
{
    return "***" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1() + "***";
}

struct ChunkResult {
    int status = 0;
    bool bodySent = false;
};

ChunkResult sendChunk(ChomikujClient &client,
                      const QString &url,
                      const QString &remoteName,
                      const QByteArray &data,
                      qint64 start,
                      qint64 end,
                      const std::function<bool()> &cancelled)
{
    throwIfCancelled(cancelled);

    const QByteArray multipartBoundary = boundary();
    const QByteArray escaped = escapeFilename(remoteName).toUtf8();

    QByteArray body;
    body.reserve(data.size() + 256 + escaped.size());
    body += "--" + multipartBoundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"files[]\";filename=\"";
    body += escaped;
    body += "\"\r\n\r\n";
    body += data;
    body += "\r\n--" + multipartBoundary + "--\r\n";

    HttpRequest request;
    request.method = QByteArrayLiteral("POST");
    request.url = QUrl(url);
    request.body = body;
    request.timeoutMs = s_chunkTimeoutMs;
    request.cancelled = cancelled;
    request.headers.insert("User-Agent", client.userAgent().toUtf8());
    request.headers.insert("Connection", "Keep-Alive");
    request.headers.insert("ENCTYPE", "multipart/form-data");
    request.headers.insert("Content-Type", "multipart/form-data;boundary=" + multipartBoundary);
    request.headers.insert("Content-Length", QByteArray::number(body.size()));
    request.headers.insert("File-Range",
                           QByteArray::number(start) + '-' + QByteArray::number(end));

    const HttpResponse response = client.transport().send(request);
    if (response.isNetworkError()) {
        if (isCancelled(cancelled)) {
            throwIfCancelled(cancelled);
        }
        return {0, response.requestBodySent};
    }
    return {response.status, response.requestBodySent};
}

bool uploadPresent(ChomikujClient &client, qint64 folderId,
                   const QString &remoteName, qint64 size,
                   const std::function<bool()> &cancelled)
{
    if (isCancelled(cancelled)) {
        return false;
    }
    try {
        const QList<Entry> entries = client.listFolder(folderId);
        const qint64 expected = expectedStoredSize(size);
        for (const Entry &entry : entries) {
            if (!entry.isDir && entry.name == remoteName && entry.sizeBytes == expected) {
                return true;
            }
        }
    } catch (const Error &) {
        // The listing is only a best-effort confirmation after an ambiguous
        // chunk failure; reconciliation below is authoritative.
    }
    return false;
}

// Asks the server where an upload really stands after an ambiguous failure.
// Returns true if the upload is complete, false if it should continue at
// `chunk` (which is updated together with `url`).
bool reconcile(ChomikujClient &client,
               const QString &remoteName,
               qint64 size,
               qint64 folderId,
               const QString &crc32Hex,
               qint64 confirmed,
               QString *url,
               qint64 *chunk,
               int *recoveries,
               const std::function<bool()> &cancelled)
{
    if (++(*recoveries) > s_maxReconcileAttempts) {
        throw Error(KIO::ERR_CONNECTION_BROKEN,
                    QStringLiteral("The upload state could not be recovered"));
    }

    qint64 previous = confirmed;
    int stable = 0;

    for (int attempt = 1; attempt <= s_maxReconcilePolls; ++attempt) {
        sleepInterruptible(retryDelayMs(attempt - 1), cancelled);
        throwIfCancelled(cancelled);

        UploadSlot slot;
        try {
            slot = client.negotiateUpload(remoteName, size, folderId, crc32Hex);
        } catch (const Error &error) {
            if (isCancelled(cancelled)) {
                throwIfCancelled(cancelled);
            }
            const int status = error.httpStatus();
            const bool permanent = error.businessCode() > 0
                || (status >= 400 && status < 500
                    && status != 408 && status != 409 && status != 429);
            if (permanent) {
                throw;
            }
            continue;
        }

        if (slot.completed) {
            return true;
        }
        if (slot.url.isEmpty()) {
            throw Error(KIO::ERR_WORKER_DEFINED,
                        QStringLiteral("Chomikuj upload returned no upload URL"));
        }

        *url = slot.url;
        *chunk = qBound<qint64>(0, slot.chunk, size);

        if (*chunk >= size) {
            return false;
        }

        if (*chunk == previous) {
            ++stable;
        } else {
            stable = 0;
            previous = *chunk;
        }

        if (stable >= 2) {
            return false;
        }
    }

    throw Error(KIO::ERR_CONNECTION_BROKEN,
                QStringLiteral("The upload state could not be recovered"));
    return false;
}

} // namespace

QString crc32Hex(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw Error(KIO::ERR_CANNOT_READ,
                    QStringLiteral("Cannot read %1").arg(path));
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    while (!file.atEnd()) {
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            throw Error(KIO::ERR_CANNOT_READ,
                        QStringLiteral("Cannot read %1").arg(path));
        }
        crc = crc32(crc, reinterpret_cast<const Bytef *>(block.constData()),
                    static_cast<uInt>(block.size()));
    }

    return QString::number(static_cast<quint32>(crc), 16);
}

QString escapeFilename(const QString &name)
{
    QString escaped;
    escaped.reserve(name.size());
    for (const QChar character : name) {
        if (QStringLiteral("?:<>/*\"\\").contains(character)) {
            escaped.append(QLatin1Char('_'));
        } else {
            escaped.append(character);
        }
    }
    return escaped;
}

void uploadFile(ChomikujClient &client,
                const QString &localPath,
                qint64 folderId,
                const QString &remoteName,
                const ProgressCallback &progress,
                const std::function<bool()> &cancelled)
{
    throwIfCancelled(cancelled);

    QFileInfo info(localPath);
    const qint64 size = info.size();
    const QString crc = crc32Hex(localPath);

    UploadSlot slot = client.negotiateUpload(remoteName, size, folderId, crc);
    if (slot.completed) {
        if (progress) {
            progress(size, size);
        }
        return;
    }
    if (slot.url.isEmpty()) {
        throw Error(KIO::ERR_WORKER_DEFINED,
                    QStringLiteral("Chomikuj upload returned no upload URL"));
    }

    QString url = slot.url;
    qint64 offset = qBound<qint64>(0, slot.chunk, size);
    int terminalResponses = 0;
    int recoveries = 0;

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw Error(KIO::ERR_CANNOT_READ,
                    QStringLiteral("Cannot read %1").arg(localPath));
    }

    for (int iteration = 0; iteration < 100000; ++iteration) {
        throwIfCancelled(cancelled);

        const bool complete = offset >= size;
        QByteArray data;
        if (!complete) {
            if (!file.seek(offset)) {
                throw Error(KIO::ERR_CANNOT_READ,
                            QStringLiteral("Cannot seek in %1").arg(localPath));
            }
            const qint64 toRead = qMin<qint64>(512 * 1024, size - offset);
            data = file.read(toRead);
            if (data.size() != toRead) {
                throw Error(KIO::ERR_CANNOT_READ,
                            QStringLiteral("Unexpected end of file during upload"));
            }
        }

        const qint64 start = offset;
        const qint64 end = complete ? start : start + data.size();
        const ChunkResult chunkResult = sendChunk(client, url, remoteName, data, start, end, cancelled);
        const int status = chunkResult.status;

        if (status == 200) {
            if (progress) {
                progress(size, size);
            }
            return;
        }

        if (status == 206) {
            if (complete) {
                ++terminalResponses;
                if (terminalResponses >= s_maxTerminalResponses) {
                    if (progress) {
                        progress(size, size);
                    }
                    return;
                }
                continue;
            }
            offset = end;
            if (progress) {
                progress(offset, size);
            }
            continue;
        }

        const bool ambiguous = status == 0 || status == 408 || status == 409
            || status == 410 || status == 429 || (status >= 500 && status < 600);

        if (ambiguous) {
            throwIfCancelled(cancelled);
            const bool finalChunk = end >= size;
            const qint64 confirmed = offset;

            // A final chunk whose body was fully handed to the transport is
            // submitted even if the response timed out.  The server may take
            // minutes to finalize a large file after accepting the last byte.
            if (finalChunk && status == 0 && chunkResult.bodySent) {
                if (progress) {
                    progress(size, size);
                }
                return;
            }

            if (finalChunk && status != 0
                && uploadPresent(client, folderId, remoteName, size, cancelled)) {
                if (progress) {
                    progress(size, size);
                }
                return;
            }

            const bool completedNow = reconcile(client, remoteName, size, folderId, crc,
                                                confirmed, &url, &offset, &recoveries,
                                                cancelled);
            if (completedNow) {
                if (progress) {
                    progress(size, size);
                }
                return;
            }

            if (offset < confirmed
                && uploadPresent(client, folderId, remoteName, size, cancelled)) {
                if (progress) {
                    progress(size, size);
                }
                return;
            }

            terminalResponses = 0;
            if (progress) {
                progress(offset, size);
            }
            continue;
        }

        if (status >= 400) {
            throw Error(kioCodeForHttp(status),
                        QStringLiteral("Upload failed (HTTP %1)").arg(status),
                        status);
        }

        // Any other 2xx is treated as a successful completion.
        if (progress) {
            progress(size, size);
        }
        return;
    }

    throw Error(KIO::ERR_CONNECTION_BROKEN,
                QStringLiteral("Upload did not make progress"));
}

} // namespace Chomikuj

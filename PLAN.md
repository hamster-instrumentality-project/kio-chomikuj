# kio-chomikuj — implementation plan

## 0. Implementation status

Implemented in this repository:

- KF6 KIO worker plugin (`chomikuj.so`) with protocol metadata
- MD5 request signing, HTTP transport, API client, pagination, error mapping,
  retry/backoff and 401 re-login handling
- KIO auth dialog / cached authentication (KWallet-backed), device id config
- `listDir`, `stat`, `mimetype`, `get`
- `put` (delete-then-upload for `Overwrite`), `mkdir`, `rename`/move, `del`
  with non-recursive-delete safety
- server-side-copy emulation via download + upload in `copy()`
- CRC32 + 512 KiB chunked upload with reconciliation after ambiguous failures
- offline unit tests, plugin-registration test and a gated self-cleaning live
  check
- a local `kioclient` smoke test against a mock server has been run manually

Decisions taken for this implementation (the recommended defaults from the
original plan): KF6 only, account selected by URL host, full read/write,
KIO password dialog + wallet-backed cached authentication, overwrite only when
KIO explicitly passes `Overwrite`.

Still open / intentionally deferred:

- public-account browsing (`chomikuj://otheruser/`) and folder-password prompts
- upload resume through KIO `put`
- a dedicated KCM/account manager
- a KIO docbook/man page and packaged translations

## 1. Goal

Implement a native KIO worker (kioworker) for Chomikuj.pl, backed by the
**Mobile API v3** documented in `../chomikuj-android-re2/MOBILE_PROTOCOL.md`.

The worker should make Chomikuj usable from Dolphin, file dialogs and other
KIO-aware applications:

- browse folders (`listDir`, `stat`)
- download files (`get`)
- upload files (`put`)
- create folders (`mkdir`)
- rename and move files/folders (`rename`)
- delete files/folders (`del`), with recursive-delete support
- report MIME types and sizes where the API allows it

The protocol's limitations are treated as first-class behaviour, not bugs:

- no file/folder timestamps
- sizes are reported by the server in **floored kilobytes**
- uploads never overwrite; overwrite must be implemented as delete-then-upload
- there is no server-side filesystem copy (only a cross-account "save a copy")
- hidden/adult/public-upload flags are read-only
- folder deletion is recursive server-side, so the worker must enforce KIO's
  non-recursive-delete contract itself

## 2. Target platform and technology

- **KDE Frameworks 6 / KIO 6** (installed here: KIO 6.31, Qt 6.12).
- C++20, CMake, ECM (`extra-cmake-modules`).
- `KIO::WorkerBase` + `KIO::WorkerResult` (not the legacy KF5 `SlaveBase`).
- Qt6 `QNetworkAccessManager` for HTTP, `QEventLoop` for synchronous worker
  calls, `QCryptographicHash` for the MD5 request signature.
- zlib for CRC32 (upload negotiation requires `Hash` = CRC32 hex).
- `KF6::I18n` for user-visible messages.
- KIO password dialog + `checkCachedAuthentication()` / `cacheAuthentication()`
  for credentials (KWallet-backed); no plaintext password in config files.
- `KSharedConfig` (`chomikujrc`) only for non-secret state: last account,
  device id/name.

KF5 is out of scope unless requested; supporting both would add a second
`SlaveBase` implementation and a second build matrix.

## 3. Proposed architecture

The protocol logic is kept independent of KIO so it can be unit-tested without
a running worker.

```
src/
  core/
    chomikujtypes.h          protocol DTOs (Folder, File, Listing, ...)
    signing.{h,cpp}          secret decoding + MD5 Token header
    httptransport.{h,cpp}    abstract Request/Response transport interface
    qthttptransport.{h,cpp}  QNetworkAccessManager implementation
    chomikujclient.{h,cpp}   endpoint methods, auth, retries, pagination
    session.{h,cpp}          login, ApiKey, KIO auth dialog, 401 re-login
    filesystem.{h,cpp}       path -> id resolution, cache, KIO-neutral ops
    uploader.{h,cpp}         CRC32, multipart framing, 512 KiB chunk loop
    downloader.{h,cpp}       signed URL + Range/refresh logic
  worker/
    chomikujworker.{h,cpp}   KIO::WorkerBase adapter, UDSEntry mapping
    kdemain.cpp              plugin entry point (or in worker .cpp)
  chomikuj.json              KIO plugin/protocol metadata
autotests/
  test_sign.cpp
  test_client.cpp
  test_models.cpp
  test_upload.cpp
  test_filesystem.cpp
  live_check.cpp             gated live verification, read-only by default
CMakeLists.txt
README.md
LICENSES/
po/                          optional
```

`ChomikujClient` and `ChomikujFileSystem` expose blocking methods returning a
small `Result<T>`/`Error` type. The worker translates those errors to KIO error
codes. This keeps the worker thin and makes the core testable with a fake
`HttpTransport`.

## 4. URL scheme and account model (recommended)

Use the URL host as the Chomikuj account name:

- `chomikuj://<account>/<path>` — that account's tree.
- `chomikuj:/` — redirect to the last successfully used account, or prompt for
  an account name/password and then redirect.
- `chomikuj:/<path>` — same, using the last account (or prompt if none).

Own-account paths are writable. Other accounts are browsed through
`GET /folders?...&AccountId=<numeric id>` and are **read-only** by default.
The numeric id is resolved once via `GET /account/search?Query=<account>` and
cached.

This gives multi-account support without a custom KCM, and keeps one worker
instance per account (KIO keys workers by protocol/host).

Alternative, simpler v1: only `chomikuj:/` for the last/primary account.
This is a decision point (see §11).

## 5. Protocol implementation notes

### 5.1 Signing and HTTP

Port `poc/client.py` semantics exactly:

```
Token = MD5( (path + ["?" + query] + body + SECRET).substring(1) )
```

- `path` includes the leading `/`; strip it before hashing.
- Query must be percent-encoded the same way on the wire and in the signature
  (UTF-8, `%20` for spaces, uppercase hex). Build the query string manually
  rather than relying on `QUrlQuery`'s form encoding.
- Sign the exact JSON bytes that are sent (`QJsonDocument::Compact`, UTF-8, no
  trailing newline). Key order is irrelevant because the server re-hashes the
  received body.
- Secret: `wzrwYua$.DSe8suk!`'2` (decoded from the APK's `BuildConfig`).
- Headers: `User-Agent: android/3.8.4 (<deviceId>; <deviceName>)`,
  `Api-Key`, `Token`, `Accept: application/json`,
  `Content-Type: application/json; charset=utf-8` for bodies.
- Success codes are `0`, `200`, `201`. Inspect the JSON `Code` even for HTTP
  2xx.
- Retry idempotent requests (GET/DELETE) on network errors and 5xx; retry
  `429` for every method; exponential backoff and `Retry-After`.
- Treat HTTP 401 on an authenticated request as session expiry: clear the
  in-memory `ApiKey`, re-login once, retry the request once.

### 5.2 Authentication

- First operation: `checkCachedAuthentication(AuthInfo)`; if absent,
  `openPasswordDialog(AuthInfo)`. `AuthInfo.url` is
  `chomikuj://<account>/`, `realmValue` is `chomikuj` (folder passwords use a
  per-folder realm).
- If the user ticks "remember", call `cacheAuthentication()`; KIO stores it in
  KWallet.
- `POST /account/login` with plaintext password over TLS, as the official app
  does. Store only the returned `ApiKey` in memory for the worker's lifetime.
- Persist a stable random `DeviceId` in `chomikujrc`; `DeviceName` defaults to
  `KDE` (configurable later).
- Do not store the password or `ApiKey` in plain text on disk.

### 5.3 Reading

- `listDir`: resolve path to folder id, then `GET /folders?Parent=<id>&page=N`
  until `IsNextPageAvailable` is false. Stop at page 50 (server cap) and warn.
- `stat`: there is no stat-by-id endpoint. Resolve the parent folder, list it,
  and find the exact name. Cache listings for the worker lifetime.
- `get`: `GET /files/download?FileId=<id>` → `FileUrl`, then stream the raw URL.
  Set `mimeType()` and `totalSize()` before data. Refresh the signed URL once on
  401/403/404/410. Check `wasKilled()` while streaming.
- Range/resume: protocol supports `Range: bytes=N-`; implement if KIO's
  `range-start`/`canResume()` semantics verify cleanly, otherwise full download
  in v1.
- UDS entries:
  - `UDS_NAME`/`UDS_DISPLAY_NAME`: `FileName` + `.` + `FileType`, with
    normalisation for the rename quirk (avoid `name.ext.ext`)
  - `UDS_FILE_TYPE`: `S_IFDIR` / `S_IFREG`
  - `UDS_SIZE`: `Size * 1024` (documented approximation)
  - `UDS_MIME_TYPE`: `QMimeDatabase` by name; listing's `MediaType` can be used
    as a hint
  - `UDS_ACCESS`: owner read/write for own tree; read-only for other accounts
  - `UDS_HIDDEN` for `IsHidden`
  - no `UDS_MODIFICATION_TIME`/`UDS_ACCESS_TIME`/`UDS_CREATION_TIME`
  - add the required `.` directory entry in `listDir`

### 5.4 Writing

- `mkdir`: `POST /folders/create` with `FolderName`, `ParentId`; map
  `Code 5` (`NameExistsAtDestination`) to `ERR_DIR_ALREADY_EXIST`.
- `put`: stream incoming KIO data to a `QTemporaryFile` (also computing CRC32),
  then:
  1. resolve destination; if it exists and `KIO::Overwrite` is not set →
     `ERR_FILE_ALREADY_EXIST`; if it is set → delete the existing file first
  2. `POST /files/upload/partialUpload`
     `{Name, Size, FolderId, Hash}`
  3. POST 512 KiB `multipart/form-data` chunks to the returned `Url` with
     `File-Range: start-end`; handle 206/200/408/409/410/5xx exactly like
     `poc/upload.py`; final empty chunk with `File-Range: size-size`
  4. report `processedSize`/`totalSize`; optionally re-list to verify
- `rename`/move:
  - same parent → `/files/changeName` or `/folders/changeName`
  - different parent → `/files/move` with `SourceFolderId`,
    `TargetFolderId`, `Files`/`Folders`, then rename if the basename changed
  - check destination existence and honour `KIO::Overwrite`
  - prevent moving a folder into itself/descendant
  - cross-account move is unsupported (`ERR_UNSUPPORTED_ACTION`)
- `del`: files → `/files/delete` `Files:[id]`; folders → check empty unless
  `metaData("recurse") == "true"`, then `/files/delete` `Folders:[id]`
  (server cascades).
- `copy`: there is no server-side copy, so download to a temporary file and
  upload it again. Directory copies are left to KIO's recursive copy machinery.
- `chmod`, `chown`, `setModificationTime`, `symlink`, `setAttributes`: not
  supported (`ERR_UNSUPPORTED_ACTION`).
- `fileSystemFreeSpace`: defer until the units of `QuotaLeft` /
  `QuotaPeriodical` / `QuotaAdditional` are verified live. Do not report
  guessed byte counts.

### 5.5 Public accounts and folder passwords (phase 3)

- `chomikuj://<other>/...`: resolve numeric `AccountId` via
  `/account/search?Query=<name>` (exact match), list with `AccountId`, expose
  read-only access.
- Password-protected folders: on HTTP 401 with `Code 12`, prompt with a
  per-folder realm, call `POST /folders/password`, retry once.
- Own folders' password gate is a no-op for the owner, so do not prompt there.

## 6. KIO protocol metadata

`chomikuj.json` (embedded into the plugin via `K_PLUGIN_METADATA`):

```json
{
  "KDE-KIO-Protocols": {
    "chomikuj": {
      "Class": ":internet",
      "ExtraNames": [],
      "Icon": "folder-remote",
      "X-DocPath": "kioworker6/chomikuj/index.html",
      "deleteRecursive": true,
      "deleting": true,
      "input": "none",
      "linking": false,
      "listing": ["Name", "Type", "Size", "Access", "Owner"],
      "makedir": true,
      "moving": true,
      "opening": false,
      "output": "filesystem",
      "protocol": "chomikuj",
      "reading": true,
      "writing": true
    }
  }
}
```

Build installs the plugin as `chomikuj.so` under `${KDE_INSTALL_PLUGINDIR}/kf6/kio`
using `kcoreaddons_add_plugin(... INSTALL_NAMESPACE kf6/kio)`.

## 7. Milestones

### M0 — scaffold and protocol core (1–1.5 days)

- CMake/ECM project, plugin metadata, minimal worker that loads.
- `signing`, `httptransport`, `qthttptransport`, DTOs, `ChomikujClient`.
- Known-answer signing tests and fake-transport client tests.
- Acceptance: `cmake --build` succeeds; `ctest` green; plugin appears in
  `kio`'s protocol list.

### M1 — read-only own account (1.5–2 days)

- Session/auth dialog, login, account id, device id.
- Path resolver/cache, `listDir`, `stat`, `get`, MIME type.
- Acceptance: browse nested folders in Dolphin, download a file and verify it
  byte-for-byte, sizes match the server's KB values, no timestamp columns are
  shown stale.

### M2 — write support (2–2.5 days)

- `mkdir`, `put` (new + overwrite), `rename`/move, `del` with recursive flag.
- Upload state machine with CRC32/multipart, progress reporting.
- Acceptance: create → upload → list → rename → move → download → delete
  round-trip; non-empty folder delete fails without recurse and succeeds with
  recurse; uploading the same name without overwrite does not silently create
  `name(1).ext`.

### M3 — multi-account/public + folder passwords (1–1.5 days, optional)

- Host-based accounts, numeric account resolution, read-only public trees,
  password prompt for protected folders.
- Acceptance: `chomikuj://otheruser/` lists public content; write operations
  are denied; protected folder opens after password.

### M4 — hardening, docs, CI (1–2 days)

- Retry/backoff, 401 re-login, URL refresh, cancel/kill handling, error
  messages, i18n.
- README (build/install/usage/limitations), protocol doc link, man page or
  KIO docbook, gated live test.
- CI (Arch container: `base-devel cmake extra-cmake-modules qt6-base kio
  ki18n zlib`), clang-format, optional ASAN/UBSAN.
- Acceptance: CI green; live read-only checks pass; `--write` live round-trip
  passes with a throwaway account.

## 8. Testing plan

### Offline unit tests

- **Signing**: secret decode equals `wzrwYua$.DSe8suk!`'2`; login, GET query and
  encoded-query known-answer vectors from `tests/test_sign.py`.
- **Client**: header assembly, JSON body bytes, query encoding, success-code
  handling, HTTP/business error mapping, retry policy, pagination cap.
- **Models**: folder/file DTO parsing, `FileName`+`FileType` composition,
  KB→bytes, missing fields.
- **Uploader**: CRC32 (`123456789` → `cbf43926`, empty → `0`), filename
  escaping, multipart framing, 206 progression, empty terminal chunk,
  409 reset, transient retry, zero-length file, progress, already-completed.
- **Filesystem**: path resolution, parent listing for `stat`, pagination,
  rename/move/delete argument shapes, overwrite safety, recursive-delete
  guard, cache invalidation.

### Local integration

- A minimal `QTcpServer`-based mock can exercise the real `QNetworkAccessManager`
  transport (signature, multipart, streaming, 401 refresh) without credentials.
- Optional KIO smoke test: `KIO::listDir`/`KIO::get` against the mock via a
  custom protocol URL, or `kioclient6` in a scripted environment.

### Gated live verification

Port the useful parts of `tests/live_check.py` to a C++ executable:

- read-only by default: login, account info, list root, stat, download URL
- `--write`: create a temp folder, upload a small file, list, rename, move,
  download and compare, delete; self-cleaning
- `--keep`, `--no-verify` flags
- credentials from `CHOMIKUJ_ACCOUNT` / `CHOMIKUJ_PASSWORD`
- exit non-zero on failure

Live checks are the only way to settle the remaining protocol questions:
rename/extension behaviour, quota units, `ApiKey` expiry, rate limiting.

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| File rename stores the full name including extension, unlike upload | Normalise `FileName`/`FileType` when composing names; verify live; never emit `name.ext.ext` |
| `Size` is floored KB, so small files show 0 and sizes are approximate | Document; use `Content-Length` for `totalSize` in `get`; do not promise byte-exact listing sizes |
| No timestamps anywhere | Omit time UDS fields; document that Dolphin date columns stay empty |
| Upload never overwrites | Delete-then-upload only when KIO passes `Overwrite`; otherwise fail with `ERR_FILE_ALREADY_EXIST` |
| Folder delete cascades server-side | Check emptiness for non-recursive `del`; only cascade when `recurse=true` |
| Signed download URLs are single-use / can expire | Re-request `/files/download` and retry once on 401/403/404/410 |
| `ApiKey` expiry semantics unverified | Re-login once on 401; if login fails, surface `ERR_CANNOT_LOGIN` |
| Large uploads and downloads | Temp file + 512 KiB chunks; stream downloads; check `wasKilled()`; add Range resume if KIO semantics verify |
| No server-side copy | Download to a temp file and upload it again; delegate directory recursion to KIO |
| Quota units unknown | Omit `fileSystemFreeSpace` until verified |
| Blocking nested event loops in worker | Timeouts, abort handling, no UI work inside request loops |
| Path resolution costs a parent listing | Cache listings per worker; clear on mutation |
| Protocol/secret pinned to app 3.8.4 | Keep constants and protocol notes together; document that a server/app update may break it |
| Unofficial client / account safety | Clear README disclaimer; use only with one's own account; never log passwords/ApiKey |

## 10. Definition of done

- KIO worker builds and installs on KF6.
- `chomikuj:/` (or `chomikuj://account/`) browses the account, downloads,
  uploads, creates folders, renames/moves and deletes.
- Errors are actionable KIO errors, not crashes or silent failures.
- Offline test suite passes; live read-only check passes; write round-trip
  passes with a throwaway account.
- README documents setup, URL scheme, capabilities and all protocol
  limitations.
- No secrets are written to disk; password handling uses KIO/KWallet.
- Code follows KDE style, SPDX headers and clang-format.

## 11. Open questions

1. **KIO target**: KF6 only (recommended, matches this machine), or also KF5?
2. **Account model**: host-based multi-account/public browsing
   (`chomikuj://account/...`, recommended), or only the last/primary account
   under `chomikuj:/` for v1?
3. **Scope**: full read/write in the first release (recommended), or a
   read-only MVP first?
4. **Public accounts / password-protected folders**: include in v1, or defer to
   phase 3?
5. **Credentials**: KIO password dialog + KWallet cache (recommended), or do
   you want a dedicated KCM/account manager and/or cached `ApiKey`?
6. **Overwrite policy**: delete-then-upload when KIO passes `Overwrite`
   (recommended, matches PoC), or refuse overwrite entirely?
7. **Live testing**: do you have a throwaway Chomikuj account whose credentials
   can be used for gated live tests? This is needed to verify rename semantics,
   quota units, 401 behaviour and upload edge cases.
8. **Licensing/metadata**: GPL-2.0-or-later (recommended, like kio-gdrive),
   and which maintainer name/email should go in CMake/SPDX headers?

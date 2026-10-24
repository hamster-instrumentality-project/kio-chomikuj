# kio-chomikuj

A native KIO worker for [Chomikuj.pl](https://chomikuj.pl), implemented on top
of the service's **Mobile API v3** as documented in the sibling
`chomikuj-android-re2` repository (`MOBILE_PROTOCOL.md`).

The worker makes a Chomikuj account usable from Dolphin, file dialogs and other
KIO-aware applications:

- browse folders and files
- download files
- upload files
- create folders
- rename and move files/folders
- delete files and folders (including recursive delete)
- report MIME types and approximate sizes

> This is an independent, unofficial client. Use it only with your own account
> and at your own risk. The protocol is reverse-engineered and may change when
> Chomikuj updates its Android app.

## URL scheme

```
chomikuj://<account>/<path>
```

Examples:

```
chomikuj://myaccount/
chomikuj://myaccount/Music
chomikuj://myaccount/Music/song.mp3
```

`chomikuj:/` uses the last successfully used account. If none has been
configured yet, open `chomikuj://<account>/` once; the account is remembered.
Credentials are requested through the standard KIO password dialog; if
"remember" is enabled, KIO stores them in KWallet.

Browsing another user's public tree is read-only:

```
chomikuj://someuser/
chomikuj://someuser/SomeFolder
```

The worker signs in with your own configured account and resolves `someuser`
through the account search endpoint. Sign in to your own account once before
using public-account URLs. Password-protected public folders prompt for the
folder password and reopen automatically.
Passwords and session keys are never written to plain-text configuration files.

The URL host selects the account being browsed. Writes are only allowed when
that account is also the account you are signed in as. The account field in the
KIO login dialog is honoured: changing it signs in as that account. For
`chomikuj:/`, the browsed account follows the signed-in account; for
`chomikuj://account/`, the host stays the browsed account, so signing in as a
different account makes it read-only.

The device id used in the `User-Agent` and the last account are stored in
`~/.config/chomikujrc`; they can be edited in the Chomikuj.pl control module.

## Build and install

Requirements:

- KDE Frameworks 6: KIO, I18n, Config, KCMUtils, DocTools
- Qt 6.5 or newer: Core, Network, Widgets
- CMake 3.20 or newer
- extra-cmake-modules (ECM)
- zlib
- gettext (`msgfmt`)
- a C++20 compiler and Ninja or Make

On Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules \
    qt6-base kio ki18n kcmutils kdoctools gettext zlib
```

### System-wide install (recommended)

Install into `/usr` so that KIO and System Settings find the plugins without
extra environment variables:

```bash
cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The installed files are (on a typical Qt6/Arch layout):

```
/usr/lib/qt6/plugins/kf6/kio/chomikuj.so
/usr/lib/qt6/plugins/plasma/kcms/systemsettings_qwidgets/kcm_chomikuj.so
/usr/share/applications/kcm_chomikuj.desktop
/usr/share/doc/HTML/en/kioworker6/chomikuj/
/usr/share/locale/pl/LC_MESSAGES/kio6_chomikuj.mo
```

### User-local install

For a build that does not touch `/usr`, use a prefix such as `$HOME/.local`
and point Qt and XDG at it:

```bash
cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
cmake --install build

export QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins:$QT_PLUGIN_PATH"
export XDG_DATA_DIRS="$HOME/.local/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
```

The environment variables must also be visible to the KIO daemon (`kiod6`), not
only to the application. A system-wide install is therefore the simplest
option.

### Verify

```bash
# The protocol should be registered
kioclient ls "chomikuj://your-account/"

# The control module should be listed
kcmshell6 --list | grep kcm_chomikuj

# The plugin advertises the local-file copy path used for upload progress
ctest --test-dir build -R tst_plugin --output-on-failure
```

After installing, restart applications that cache KIO protocol information
(Dolphin, file dialogs, etc.) and, if necessary, log out and back in so the KIO
daemon (`kiod6`) picks up the new plugin. The control module appears in System
Settings under **Network → Chomikuj.pl**, where the default account, device name
and device ID can be configured.

### Upload progress troubleshooting

For a local file copied to Chomikuj, progress should advance with the actual
chunked upload. If it races to 100% almost immediately and then sits still, KIO
is still using the old data-pump path, where the progress bar tracks reading the
local source file instead. Check that:

- the current build is actually installed or selected through `QT_PLUGIN_PATH`;
- no older `chomikuj.so` is earlier in the plugin path;
- `kiod6` and the file manager were restarted after installing;
- `ctest --test-dir build -R tst_plugin` passes, which verifies that
  `KProtocolManager::canCopyFromFile()` is true for `chomikuj://`.

KIO's `copyFromFile` optimisation only applies when the source is a `file://`
URL. Copying from SMB, MTP, archives or other workers still uses KIO's data
pump, whose progress reflects reading the source rather than the Chomikuj
upload.

## Testing

Offline unit tests (no credentials and no network):

```bash
ctest --test-dir build --output-on-failure
```

They cover request signing, response/error parsing, pagination, the chunked
upload state machine, CRC32/multipart framing, path resolution, rename/move and
delete safety, and plugin metadata registration.

A gated live check talks to the real backend and only touches resources it
creates itself:

```bash
export CHOMIKUJ_ACCOUNT=your_account
export CHOMIKUJ_PASSWORD='your_password'

# read-only: login + root listing
./build/bin/chomikuj_live

# also run a self-cleaning create/upload/rename/move/download/delete round-trip
./build/bin/chomikuj_live --write

# read-only public-account listing check
./build/bin/chomikuj_live --public someuser
```

For local integration testing, the worker honours:

- `CHOMIKUJ_BASE_URL` — override the API base URL (e.g. a mock server)
- `CHOMIKUJ_ACCOUNT` / `CHOMIKUJ_PASSWORD` — test credentials, bypassing the
  password dialog
- `CHOMIKUJ_RETRY_BASE_MS` / `CHOMIKUJ_RETRY_MAX_MS` — override the retry
  backoff window (defaults: 500 ms doubling to 8 s, with full jitter)

These are intended for development and CI only.

## Protocol limitations

The Mobile API has several limitations which are reflected in the worker rather
than hidden:

- **No timestamps.** Folders and files have no modification/creation time.
  Date columns stay empty.
- **Kilobyte-granularity sizes.** Listings report `Size` in floored kilobytes.
  `get()` reports the exact size from `Content-Length` when downloading.
- **No overwrite on upload.** Uploading an existing name creates
  `name(1).ext`. When KIO passes the `Overwrite` flag, the worker uploads the
  replacement under a temporary name, deletes the old file only after the
  upload succeeded, and then renames the new file into place.
- **No server-side copy.** `copy()` downloads the file to a temporary file and
  uploads it again. Directory copies are delegated to KIO's recursive copy
  machinery.
- **No storage quota.** The transfer balance is not a byte count, so
  `fileSystemFreeSpace()` deliberately leaves total/available unset.
- **No attribute changes.** Hidden/adult/public-upload flags are read-only;
  `chmod`, `chown`, `setModificationTime` and symlinks are unsupported.
- **Recursive folder delete is server-side.** The worker refuses to delete a
  non-empty folder unless KIO sends `recurse=true`.
- **No resume on upload.** Chunked upload is resumable at the protocol level,
  but the KIO `put` implementation reads the stream into a temporary file and
  uploads it from the beginning in the current version.
- **Public accounts are read-only.** `chomikuj://otheruser/` works for public
  trees, but write operations are refused locally before any API call.
- **Folder passwords** are supported through the standard KIO password dialog.

## Implementation

- `src/core/` — protocol client, signing, HTTP transport, path resolver,
  chunked uploader. Kept independent of KIO for testability.
- `src/worker/` — `KIO::WorkerBase` adapter and UDSEntry mapping.
- `autotests/` — offline tests and the gated live check.
- `PLAN.md` — design notes, milestones and protocol risks.

The protocol reference is in
[`chomikuj-android-re2/MOBILE_PROTOCOL.md`](../chomikuj-android-re2/MOBILE_PROTOCOL.md).

## Licence

GPL-2.0-or-later. See `LICENSES/GPL-2.0-or-later.txt`.

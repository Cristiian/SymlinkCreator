# SymlinkCreator: notes for Claude

Explorer context-menu extension for Windows 11 that creates and manages symbolic links, the way
"Paste shortcut" creates shortcuts. C++20, MSVC, Windows SDK only: no CMake, no NuGet, no runtime
dependencies (static CRT `/MT`). The user-facing documentation is `README.md`; this file holds what a
contributor (or Claude) needs to change the code safely.

## Rules for this repository

- **Everything is in English**: code, comments, README, menu labels, messages, commit messages. This
  is a deliberate project decision for future contributors.
- **Commit or push only when the user asks.** `master` is the main branch; the remote is
  `https://github.com/Cristiian/SymlinkCreator`.
- **Never commit the signing key.** `*.pfx` and `build/` are ignored. Only the public `.cer` ships, and
  only inside the installer.
- Do not change system or security settings on the user's machine (Developer Mode, certificate stores,
  running the MSI). Build the MSI and let the user install it and accept UAC.
- The Windows 11 menu cannot show a third-party button in the icon row (Cut/Copy/Paste/Rename/Delete)
  or in the Explorer command bar. Top-level menu entries are the closest option. Do not try again.

## Layout

| Path | What it is |
|---|---|
| `src/common/links.hpp/.cpp` | Shared logic (namespace `symlink_creator`): create, read and replace links, clipboard, request files, error text. Used by both binaries. |
| `src/common/ids.hpp` | The five COM CLSIDs, `kPackageName` and `kPackagePublisher`. Must match `package/AppxManifest.xml`. |
| `src/shell/shell.cpp` | `SymlinkCreator.dll`: the `IExplorerCommand` handlers, `DllGetClassObject`, `DllCanUnloadNow`. |
| `src/shell/actions.hpp/.cpp` | Actions on existing links that use shell UI: open target location, copy target path, change target (`IFileOpenDialog`), convert, replace with a copy and move-and-link (`IFileOperation`). |
| `src/shell/exports.def` | DLL exports. |
| `src/app/app.cpp` | `SymlinkCreator.exe` (GUI subsystem, no console window): elevated `--apply`, command line, package registration, certificate trust. |
| `package/AppxManifest.xml` | Sparse package ("package with external location"). `${Version}` is replaced by `build.ps1`. |
| `package/Logo.svg`, `Logo.png` | Logo. The PNG is committed; delete it and rebuild to re-render it from the SVG with headless Edge. |
| `installer/Product.wxs` | WiX 6 MSI. |
| `build.ps1` | The only build script. |
| `.github/workflows/build.yml` | CI build and releases. |

## Menu commands

Registered in the manifest for three item types:

| Item type | Commands |
|---|---|
| `Directory\Background` (empty space in a folder) | PasteSymlink, PasteLink, MoveAndLink |
| `Directory` (right-click a folder) | PasteSymlink, PasteLink, MoveAndLink, OpenTarget, LinkOptions |
| `*` (any file) | OpenTarget, LinkOptions |

- **Paste as symlink** (`PasteCommand`, top level): absolute symlink in one click.
- **Paste as link ▸** (`LinkMenuCommand`): Relative symlink, Directory junction, Hard link.
- **Move here and leave a symlink** (`MoveAndLinkCommand`): only after *Cut*
  (`CFSTR_PREFERREDDROPEFFECT` = `DROPEFFECT_MOVE`). Moves with `IFileOperation` (progress, conflicts,
  elevation), maps old to new paths with an `IFileOperationProgressSink`, clears the clipboard, then
  creates absolute symlinks at the original paths.
- **Open target location** (top level) and **Link options ▸** (Copy target path, Change target…,
  Convert to relative symlink, Convert to absolute symlink, Replace with a copy of the target) appear
  only when every selected item is a symlink or junction.

Destination: the clicked folder (`Directory`), or the folder open in the window
(`Directory\Background`, read through `IObjectWithSite` → `IShellBrowser` → `IFolderView` →
`IPersistFolder2`). With more than one folder selected there is no destination.

Visibility rules (`GetState`) and the decisions behind them:

- **The clipboard keeps the last copied items** even if the user copied them long ago, just as
  Windows' own Paste stays enabled. The paste commands therefore check `CanPaste(kind, …)`. It
  requires every clipboard item to exist and to be linkable into the destination with that kind
  (`IsSupported`).
- **Top-level commands are hidden** when they cannot work. Submenu entries are **greyed out** when they
  do not apply to every copied item. The submenu itself is hidden when none of its kinds apply.
- **A folder is never linked inside itself or inside one of its subfolders.** That creates a loop
  (`C:\x\Drive C\x\Drive C\…`) that recursive tools, backups and OneDrive may follow. Copying drive
  `C:` and pasting anywhere on `C:` is blocked for the same reason. The user chose to keep this
  blocked; do not add an override.
- Packaged `IExplorerCommand` verbs also appear in the classic menu (Shift / "Show more options").
  Do not add separate classic registry verbs: they show up twice.

## Link rules (`links.cpp`)

| Kind | API | Limits | Needs admin |
|---|---|---|---|
| Symlink | `CreateSymbolicLinkW` + `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` (+ `_DIRECTORY` for folders) | none | Yes, unless Developer Mode is on |
| Relative symlink | same, target from a case-insensitive relative path | same volume | same |
| Directory junction | `FSCTL_SET_REPARSE_POINT` with a mount-point buffer | folders, local volumes | No |
| Hard link | `CreateHardLinkW` | files, same volume | No |

- Free names: `name (2).ext`, folders `name (2)`, and a drive root becomes `Drive C`.
- Paths are compared case-insensitively (`CompareStringOrdinal`). Volumes are compared by volume GUID.
- Reading links uses `FSCTL_GET_REPARSE_POINT`. The symlink flag `SYMLINK_FLAG_RELATIVE` = 1 marks
  relative symlinks. Volume mount points are not treated as links.
- **Replace** (change target, convert): the new link is created under a temporary name
  `<name>.slc-<tick>`, then the old link is removed and the new one renamed with `MoveFileExW`. Only
  reparse points are ever removed, so a failure leaves the original link intact. Without `replace`, an
  explicit `linkPath` that already exists fails with `ERROR_ALREADY_EXISTS`.
- `SHChangeNotify` runs after every change so Explorer refreshes.
- Errors are collected and shown in one message box (at most 10 listed).
- No `cmd` or PowerShell is ever used to create links, so names with `%`, `'`, `&` and so on work.

### Elevation

If a symlink fails with `ERROR_PRIVILEGE_NOT_HELD` (Developer Mode off), the DLL:
1. writes a request file in `%TEMP%` (`GetTempFileNameW` prefix `slc`);
2. runs `SymlinkCreator.exe --apply "<file>"` with `ShellExecuteExW` and the verb `runas`.

The request file is UTF-16LE with a BOM and one line per request:
`kind<TAB>source<TAB>directory<TAB>linkPath<TAB>replace(0|1)`. Every line must have exactly 5 fields.
The helper deletes the file. If the user declines UAC (`ERROR_CANCELLED`), the message suggests
Developer Mode. Junctions and hard links never elevate.

## Packaging and install

- **Why a package:** the Windows 11 context menu only loads `IExplorerCommand` handlers from packaged
  apps. A sparse package gives the DLL a package identity while the binaries stay in
  `C:\Program Files\SymlinkCreator`. The `.msix` holds only the manifest and the logo.
- **Manifest:**
  - `desktop4:FileExplorerContextMenus` + `desktop5:ItemType`;
  - `com:SurrogateServer` (dllhost) with the 5 classes, `ThreadingModel="STA"`;
  - `uap10:AllowExternalContent`, `runFullTrust`, `unvirtualizedResources`, `AppListEntry="none"`.
- **Identity:** `Name="SymlinkCreator"`, `Publisher="CN=SymlinkCreator"`. The certificate subject must
  match the publisher.
- **`--register`:** removes any existing package (`FindPackagesForUser` + `RemovePackageAsync`), then:
  - with `SymlinkCreator.msix` next to the exe: `PackageManager.AddPackageByUriAsync` with
    `ExternalLocationUri` set to the exe folder;
  - otherwise, with `AppxManifest.xml`: `RegisterPackageByUriAsync` with `DeveloperMode=true` (dev
    loop, needs Developer Mode).
- **`--unregister`:** removes the package.
- **Certificate:** `build.ps1` creates a self-signed code-signing certificate `CN=SymlinkCreator` in
  `Cert:\CurrentUser\My` (RSA 3072, 10 years). The private key stays there. The MSI runs
  `--install-cert` (`CertAddCertificateContextToStore`) into `LocalMachine\TrustedPeople`. That is
  enough for MSIX and, unlike Root, trusts nothing else. Removal uses `CERT_FIND_EXISTING`.
- **MSI (`Product.wxs`):**
  - perMachine, `MajorUpgrade`; UpgradeCode `F17ECF19-0467-4EB5-89F2-A4E441930F71` (never change it);
  - one component with a fixed GUID, because the binaries have no version resource;
  - install: `InstallCertificate` (deferred, not impersonated, i.e. elevated, after `InstallFiles`),
    then `RegisterMenu` (deferred, impersonated, as the installing user);
  - uninstall: `UnregisterMenu`, then `RemoveCertificate`;
  - only the installing user gets the menu; other users run `SymlinkCreator.exe --register`.

## Build

Requirements:
- MSVC (found through `vswhere` and `vcvars64.bat`);
- Windows SDK `10.0.26100.0` (`makeappx`, `signtool`, C++/WinRT headers);
- WiX 6.0.2 for the MSI. Locally it lives in `C:\AI_TOOLS\wix\wix.exe`; v7 needs an EULA, so stay on v6.

```powershell
.\build.ps1                                # build\bin: DLL, EXE, loose AppxManifest.xml
.\build.ps1 -Package                       # + signed SymlinkCreator.msix and SymlinkCreator.cer
.\build.ps1 -Installer -Version 1.0.2      # + build\SymlinkCreator-1.0.2.msi (implies -Package)
```

- Compiler flags: `/std:c++20 /EHsc /O2 /MT /utf-8 /permissive- /W4`, `UNICODE`.
- The DLL links: `ole32 oleaut32 shell32 shlwapi user32 runtimeobject`.
- The EXE links: `ole32 oleaut32 shell32 user32 advapi32 crypt32 runtimeobject`.
  - `oleaut32` is required by C++/WinRT (`SysFreeString`).
  - `app.cpp` needs `<winrt/Windows.ApplicationModel.h>` for `IPackage::Id`.
- `-Version` has three parts (`1.2.3`). The MSIX gets a fourth `.0`. Raise the version for every
  installer handed out; MajorUpgrade replaces the old one.
- `build.ps1` runs under Windows PowerShell 5.1. Save scripts as UTF-8 **with BOM** or keep them
  ASCII: 5.1 reads BOM-less UTF-8 as ANSI. Do not name variables after automatic variables (`$profile`).

**Development loop** (Developer Mode on):

```powershell
build\bin\SymlinkCreator.exe --unregister   # the DLL is locked by dllhost while registered
.\build.ps1
build\bin\SymlinkCreator.exe --register     # registers the loose manifest from build\bin
```

If the MSI is installed, it owns the registration (package `SymlinkCreator`). Test changes through a
new MSI (`-Installer -Version x.y.z`), or uninstall it first. Restart Explorer if menu changes do not
show: `Stop-Process -Name explorer`.

## Testing

There is no unit-test project. What has worked:

- **Link logic from the command line** (writes to the calling console):
  ```powershell
  build\bin\SymlinkCreator.exe --link <symlink|relative|junction|hardlink> <folder> <source>...
  build\bin\SymlinkCreator.exe --info <link>...
  build\bin\SymlinkCreator.exe --apply <request file>   # replace/convert paths, as the DLL does
  ```
  Check the results with `Get-ChildItem | Select-Object Name, LinkType, Target`.
- **Menu visibility without Explorer:** a small C++ program that `LoadLibrary`s
  `build\bin\SymlinkCreator.dll`, puts a `CF_HDROP` on the clipboard, gets a command through
  `DllGetClassObject`, and calls `GetState` / `EnumSubCommands` with an `IShellItemArray` built from
  a folder (`SHCreateItemFromParsingName` + `SHCreateShellItemArrayFromShellItem`). This changes the
  user's clipboard.
- **Edge cases to keep covering:**
  - names with `%`, `'`, `&` and spaces;
  - name clashes (`(2)`);
  - another drive (relative and hard link disabled);
  - a clipboard with text only (menu hidden);
  - the copied folder itself or one of its subfolders as the destination (paste commands hidden);
  - Developer Mode off (symlinks prompt UAC; junctions and hard links do not).
- Scratch test folders live in `C:\AI_TOOLS\work\slc-test` on the author's machine (not in the repo).
- Anything in Explorer itself (menus, dialogs, UAC, the MSI) is tested by the user. Say clearly what
  was and was not verified.

## CI and releases

`.github/workflows/build.yml`, on a `windows-2025` runner:
- a push to `master`, a pull request or a manual run builds the MSI as an artifact with version
  `0.0.<run number>`;
- a tag `vX.Y.Z` builds that version and a second job (`ubuntu-latest`, `contents: write`) publishes a
  GitHub release with the MSI via `gh release create --generate-notes`. Tags with any other format
  fail the build.
- Signing in CI: the secrets `SIGNING_CERT_PFX` (base64 of the `.pfx`) and `SIGNING_CERT_PASSWORD`
  are imported into `Cert:\CurrentUser\My`, and the imported subject must be `CN=SymlinkCreator`.
  Without them, `build.ps1` makes a throwaway certificate (fine for PRs, warned on tags). The README
  has the export commands.
- Step scripts use `shell: powershell` (5.1, same as local). Write to `$GITHUB_OUTPUT` with
  `[IO.File]::AppendAllText` because 5.1's `Out-File -Encoding utf8` adds a BOM.
- Action versions: `actions/checkout@v7`, `actions/upload-artifact@v7`,
  `actions/download-artifact@v8`.

## Ideas not done yet

- A `VERSIONINFO` resource for the DLL and EXE (then the WiX component could use versioned key paths).

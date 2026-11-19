# SymlinkCreator

Explorer context menu for Windows 11 that creates and manages symbolic links, the same way
"Paste shortcut" creates shortcuts. Works in the new context menu and in "Show more options".

## Usage

**Create links.** Copy (or cut) one or more files or folders. Then right-click the background of a
folder, or right-click a folder:

| Command | What it does |
|---|---|
| **Paste as symlink** | Creates an absolute symbolic link for each copied item. |
| **Paste as link ▸ Relative symlink** | Symlink whose target is stored relative to the link. Keeps working if both are moved together. Same drive only. |
| **Paste as link ▸ Directory junction** | Folders only, local drives. Never needs administrator rights. |
| **Paste as link ▸ Hard link** | Files only, same drive. Never needs administrator rights. |
| **Move here and leave a symlink** | Shown after *cut*. Moves the items here and leaves a symlink at each original location (handy for moving big folders to another drive). |

Name clashes get a suffix: `name (2).ext`. Options that do not apply to every copied item are greyed out,
and the commands are hidden when nothing on the clipboard can be linked there (for example, right-clicking
the very folder you copied, since the clipboard keeps the last copied items).

**Manage links.** Right-click a symlink or junction (or several):

| Command | What it does |
|---|---|
| **Open target location** | Opens the folder of the target with it selected. |
| **Link options ▸ Copy target path** | Copies the target path(s) to the clipboard. |
| **Link options ▸ Change target…** | Picks a new target and recreates the link with the same type. |
| **Link options ▸ Convert to relative / absolute symlink** | Rewrites the stored target. |
| **Link options ▸ Replace with a copy of the target** | Turns the link into a real copy of what it points to. |

Deleting a link with Explorer deletes only the link, never its target.

### Administrator rights

Windows only lets administrators create symbolic links, unless **Developer Mode** is on
(Settings › System › Advanced › Developer Mode). Without it, SymlinkCreator asks for permission (UAC)
whenever it creates a symlink. Junctions and hard links never ask.

## Install

Run `SymlinkCreator-<version>.msi`. It:

1. installs the files into `C:\Program Files\SymlinkCreator`,
2. trusts the package signing certificate (`LocalMachine\TrustedPeople`),
3. registers the context menu for the user who runs it.

Other users on the same PC can register it for themselves with
`"C:\Program Files\SymlinkCreator\SymlinkCreator.exe" --register`.

If the commands do not appear right away, restart Explorer (`Stop-Process -Name explorer`).
Uninstall from Settings › Apps; that removes the menu and the certificate.

## Command line

`SymlinkCreator.exe` has no console window, but writes to the console that starts it:

```
SymlinkCreator.exe --link <symlink|relative|junction|hardlink> <folder> <source>...
SymlinkCreator.exe --info <link>...
SymlinkCreator.exe --register | --unregister
SymlinkCreator.exe --install-cert <file.cer> | --remove-cert <file.cer>
```

## How it works

- `src/common/links.cpp`: link creation with the Win32 API (`CreateSymbolicLinkW`, a mount-point
  reparse buffer for junctions, `CreateHardLinkW`), reading links (`FSCTL_GET_REPARSE_POINT`) and the
  clipboard. No `cmd` or PowerShell, so any file name works.
- `src/shell/`: `SymlinkCreator.dll`, the `IExplorerCommand` handlers (`shell.cpp`) and the actions
  that use the shell copy engine and dialogs (`actions.cpp`).
- `src/app/app.cpp`: `SymlinkCreator.exe`. Creates symlinks elevated when the DLL lacks the
  privilege (`--apply <request file>`), registers the package, manages the certificate.
- `package/AppxManifest.xml`: a sparse package ("package with external location"). The Windows 11
  menu only accepts commands from packaged apps; the package holds only the manifest and logo, and the
  binaries stay in the install folder.
- `installer/Product.wxs`: the MSI (WiX 6).

COM class IDs live in `src/common/ids.hpp` and must match the manifest.

## Build

Requirements: MSVC (Visual Studio or Build Tools with "Desktop development with C++"),
Windows SDK 10.0.26100, and WiX 6 for the installer
(`dotnet tool install wix --version 6.0.2 --tool-path C:\AI_TOOLS\wix`).

```powershell
.\build.ps1                       # build\bin: DLL, EXE and a loose manifest
.\build.ps1 -Installer -Version 1.0.1   # also the signed .msix and build\SymlinkCreator-1.0.1.msi
```

Raise `-Version` for every installer you hand out.

### Releases (GitHub Actions)

`.github/workflows/build.yml` builds the MSI on a Windows runner for every push to `master` and every
pull request, and keeps it as a workflow artifact (version `0.0.<run number>`). Pushing a tag
publishes a GitHub release with the installer:

```powershell
git tag v1.0.2
git push origin v1.0.2
```

To sign every release with the same certificate, export it once and store it as two repository secrets
(Settings › Secrets and variables › Actions), `SIGNING_CERT_PFX` and `SIGNING_CERT_PASSWORD`:

```powershell
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=SymlinkCreator' |
    Sort-Object NotAfter -Descending | Select-Object -First 1
$password = Read-Host -AsSecureString 'Password for the .pfx'
Export-PfxCertificate -Cert $cert -FilePath "$env:TEMP\SymlinkCreator.pfx" -Password $password
[Convert]::ToBase64String([IO.File]::ReadAllBytes("$env:TEMP\SymlinkCreator.pfx")) | Set-Clipboard   # paste as SIGNING_CERT_PFX
Remove-Item "$env:TEMP\SymlinkCreator.pfx"
```

Without the secrets the workflow signs with a throwaway certificate. Each installer still works,
because it trusts the certificate it carries.

**Development loop** (needs Developer Mode): `.\build.ps1`, then
`build\bin\SymlinkCreator.exe --register` registers the loose manifest straight from `build\bin`.
Run `--unregister` before rebuilding so the DLL is not locked.

### Signing certificate

The first `-Package`/`-Installer` build creates a self-signed code-signing certificate
`CN=SymlinkCreator` in `Cert:\CurrentUser\My`. Its private key never leaves that store and is not
in the repository; only the public `.cer` ships in the installer. Builds from another machine get a
different certificate. That is fine, because each installer carries and trusts its own `.cer`.

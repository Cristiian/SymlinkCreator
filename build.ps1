<#
.SYNOPSIS
Builds SymlinkCreator with MSVC and the Windows SDK. No other dependencies.

.EXAMPLE
.\build.ps1              # build\bin: DLL, EXE and a loose AppxManifest.xml (register needs Developer Mode)
.\build.ps1 -Package     # also a signed SymlinkCreator.msix and the public SymlinkCreator.cer
.\build.ps1 -Installer   # also build\SymlinkCreator-<version>.msi (implies -Package; needs WiX 6)
#>
param(
    [string]$Version = '1.0.0',          # major.minor.patch
    [switch]$Package,
    [switch]$Installer,
    [string]$Sdk = '10.0.26100.0',
    [string]$Wix = 'C:\AI_TOOLS\wix\wix.exe'
)
$ErrorActionPreference = 'Stop'
if ($Installer) { $Package = $true }
$packageVersion = "$Version.0"           # MSIX versions have four parts

$root = $PSScriptRoot
$bin = Join-Path $root 'build\bin'
$obj = Join-Path $root 'build\obj'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkBin = "$kits\bin\$Sdk\x64"
$subject = 'CN=SymlinkCreator'   # must match Publisher in package\AppxManifest.xml

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $vcvars) { throw 'MSVC was not found (vcvars64.bat). Install the "Desktop development with C++" workload.' }
if (-not (Test-Path "$kits\Include\$Sdk\cppwinrt")) { throw "Windows SDK $Sdk was not found. Install it or pass -Sdk." }

function Invoke-Msvc([string]$Command) {
    # vswhere must be on PATH for vcvars64.bat to stay quiet.
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
    cmd /c "`"$vcvars`" >nul && $Command"
    if ($LASTEXITCODE) { throw "Build step failed ($LASTEXITCODE)." }
}

New-Item -ItemType Directory -Force $bin, "$obj\dll", "$obj\exe" | Out-Null

# 1. Binaries --------------------------------------------------------------------------------
$flags = "/nologo /std:c++20 /EHsc /O2 /MT /utf-8 /permissive- /W4 /DNDEBUG /DUNICODE /D_UNICODE " +
         "/I `"$kits\Include\$Sdk\cppwinrt`""
Push-Location $root
try {
    Invoke-Msvc ("cl $flags /LD src\common\links.cpp src\shell\shell.cpp src\shell\actions.cpp /Fo:`"$obj\dll\\`" " +
                 "/Fe:`"$bin\SymlinkCreator.dll`" /link /DEF:src\shell\exports.def /IMPLIB:`"$obj\SymlinkCreator.lib`" " +
                 "ole32.lib oleaut32.lib shell32.lib shlwapi.lib user32.lib runtimeobject.lib")
    Invoke-Msvc ("cl $flags src\common\links.cpp src\app\app.cpp /Fo:`"$obj\exe\\`" " +
                 "/Fe:`"$bin\SymlinkCreator.exe`" /link /SUBSYSTEM:WINDOWS " +
                 "ole32.lib oleaut32.lib shell32.lib user32.lib advapi32.lib crypt32.lib runtimeobject.lib")
} finally { Pop-Location }

# 2. Logo (the PNG is committed; delete it to render it again from the SVG with headless Edge) --
$svg = Join-Path $root 'package\Logo.svg'
$png = Join-Path $root 'package\Logo.png'
if (-not (Test-Path $png)) {
    $edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
              "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    $edgeProfile = Join-Path $obj 'edge-profile'
    Start-Process $edge -Wait -WindowStyle Hidden -ArgumentList @(
        '--headless', '--disable-gpu', "--user-data-dir=`"$edgeProfile`"", '--hide-scrollbars',
        '--default-background-color=00000000', '--window-size=256,256', "--screenshot=`"$png`"",
        ([Uri]$svg).AbsoluteUri)
    Remove-Item -Recurse -Force $edgeProfile -ErrorAction SilentlyContinue
}
Copy-Item $png $bin -Force

# 3. Package manifest ------------------------------------------------------------------------
$manifest = (Get-Content (Join-Path $root 'package\AppxManifest.xml') -Raw -Encoding UTF8).Replace('${Version}', $packageVersion)
[IO.File]::WriteAllText("$bin\AppxManifest.xml", $manifest, (New-Object Text.UTF8Encoding $false))

# 4. Signed package --------------------------------------------------------------------------
if ($Package) {
    # Self-signed code-signing certificate; the private key stays in this user's certificate store.
    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -FriendlyName 'SymlinkCreator package signing' `
            -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 -NotAfter (Get-Date).AddYears(10)
        Write-Host "Created signing certificate $($cert.Thumbprint)"
    }
    Export-Certificate -Cert $cert -FilePath "$bin\SymlinkCreator.cer" -Type CERT | Out-Null

    $staging = Join-Path $obj 'msix'
    Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $staging | Out-Null
    Move-Item "$bin\AppxManifest.xml" $staging   # the install folder uses the .msix instead
    Copy-Item $png $staging

    & "$sdkBin\makeappx.exe" pack /o /nv /d $staging /p "$bin\SymlinkCreator.msix" | Out-Null
    if ($LASTEXITCODE) { throw "makeappx failed ($LASTEXITCODE)" }
    & "$sdkBin\signtool.exe" sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My "$bin\SymlinkCreator.msix"
    if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
} else {
    Remove-Item "$bin\SymlinkCreator.msix", "$bin\SymlinkCreator.cer" -ErrorAction SilentlyContinue
}

# 5. Installer -------------------------------------------------------------------------------
if ($Installer) {
    if (-not (Test-Path $Wix)) { throw "WiX was not found at $Wix (dotnet tool install wix --version 6.0.2 --tool-path ...)." }
    $msi = Join-Path $root "build\SymlinkCreator-$Version.msi"
    & $Wix build -arch x64 -d "Version=$Version" -d "BinDir=$bin" -o $msi (Join-Path $root 'installer\Product.wxs')
    if ($LASTEXITCODE) { throw "wix build failed ($LASTEXITCODE)" }
    Remove-Item ([IO.Path]::ChangeExtension($msi, '.wixpdb')) -ErrorAction SilentlyContinue
    Write-Host "Installer: $msi"
}

Write-Host "Built SymlinkCreator $Version in $bin"

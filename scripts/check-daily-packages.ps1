param([Parameter(Mandatory=$true)][string]$CiVersion)
$ErrorActionPreference = 'Stop'
$expected = & "$PSScriptRoot/vrr-pe-version.ps1" -CiVersion $CiVersion
if ($LASTEXITCODE -ne 0 -or !$expected) { throw 'Missing daily product version' }
$installer = New-Object -ComObject WindowsInstaller.Installer
foreach ($arch in @('x64', 'arm64')) {
    $exe = (Resolve-Path "build/build-$arch-release/app/release/Moonlight.exe").Path
    $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
    if (($version.Split('.')[0..2] -join '.') -ne $expected) { throw "$arch PE version $version != $expected" }
    $msi = (Resolve-Path "build/build-$arch-release/Moonlight.msi").Path
    $database = $installer.OpenDatabase($msi, 0)
    $view = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductVersion'")
    $view.Execute()
    $record = $view.Fetch()
    if (!$record -or [version]$record.StringData(1) -ne [version]$expected) {
        throw "$arch MSI version does not match $expected"
    }
    $view.Close()
}
$bundle = (Resolve-Path "build/installer-release/MoonlightSetup-$CiVersion.exe").Path
$bundleVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($bundle).FileVersion
if (($bundleVersion.Split('.')[0..2] -join '.') -ne $expected) { throw "Bundle version $bundleVersion != $expected" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path "build/installer-x64-release/MoonlightPortable-x64-$CiVersion.zip"))
try {
    $names = @($zip.Entries.FullName)
    foreach ($file in @('Moonlight.exe', 'vrrreplay.exe', 'decode-vrr-trace.py', 'portable.dat.inactive')) {
        if ($names -notcontains $file) { throw "Portable ZIP is missing $file" }
    }
    if ($names -contains 'portable.dat') { throw 'Portable ZIP has active settings marker' }
} finally { $zip.Dispose() }
Write-Host "Daily PE, MSI, Burn and portable package gates passed: $expected"

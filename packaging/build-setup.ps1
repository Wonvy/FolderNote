param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Destination,
    [switch]$Win11
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$edition = if ($Win11) { 'win11' } else { 'standard' }
$stage = Join-Path $root "build/installer-payload/$edition"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$files = 'Install.exe','Uninstall.exe','FolderNote.exe','FolderNoteShell.dll','FolderNoteNavigation.dll'
if ($Win11) { $files += 'Install-Win11.exe','FolderNote.msix' }
if (Test-Path -LiteralPath (Join-Path $Source 'Microsoft.WindowsAppRuntime.Bootstrap.dll')) {
    $files += 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
}
$directives = @(
    '.OPTION EXPLICIT'
    '.Set CabinetNameTemplate=payload.cab'
    '.Set DiskDirectoryTemplate=.'
    '.Set Cabinet=on'
    '.Set Compress=on'
    '.Set CompressionType=LZX'
    '.Set CompressionMemory=21'
    '.Set MaxDiskSize=0'
    '.Set MaxCabinetSize=0'
    '.Set MaxDiskFileCount=0'
)
foreach ($file in $files) {
    $inputFile = Join-Path $Source $file
    if (!(Test-Path -LiteralPath $inputFile -PathType Leaf)) { throw "Missing installer payload: $file" }
    # Use local ASCII names in the DDF, including when the checkout path is Unicode.
    Copy-Item -LiteralPath $inputFile -Destination (Join-Path $stage $file) -Force
    $directives += ('"{0}" "{0}"' -f $file)
}
$directives | Set-Content -LiteralPath (Join-Path $stage 'payload.ddf') -Encoding ascii
Push-Location $stage
try {
    $cabinetLog = & "$env:SystemRoot/System32/makecab.exe" /f payload.ddf /v0 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Could not build installer cabinet: $cabinetLog" }
} finally { Pop-Location }
# Touch the resource source as well: RC dependency scanners do not always track CAB changes.
$resource = Join-Path $root "build/setup-$edition.rc"
(Get-Item -LiteralPath $resource).LastWriteTime = Get-Date
cmake --build "$root/build" --config Release --target "Setup-$edition" --parallel
if ($LASTEXITCODE -ne 0) { throw 'Could not build standalone installer.' }
Copy-Item -LiteralPath "$root/build/Release/Setup-$edition.exe" -Destination $Destination -Force

# Verify the exact shipped EXE in a fresh directory with no sibling payload files.
# This only unpacks and compares bytes and icons; it never installs into the host.
$verification = Join-Path ([IO.Path]::GetTempPath()) ("FolderNote-PackageCheck-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $verification | Out-Null
try {
    $probe = Join-Path $verification 'SetupPackageTests.exe'
    Copy-Item -LiteralPath "$root/build/Release/SetupPackageTests.exe" -Destination $probe
    & $probe $Destination $Source $edition
    if ($LASTEXITCODE -ne 0) { throw 'Standalone installer verification failed.' }
} finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($verification)
    if (!$resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid verification directory.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

param([switch]$StableNames)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$version = (Get-Item -LiteralPath "$build\Release\FolderNote.exe").VersionInfo.ProductVersion
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid FolderNote product version.' }

$out = Join-Path $dist "FolderNote-$version"
cmake --install $build --config Release --prefix $out
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the universal installer.' }

$base = if ($StableNames) { 'FolderNote' } else { "FolderNote-$version" }
$universalZip = Join-Path $dist "$base-win-x64.zip"
$win11Zip = Join-Path $dist "$base-win11-test.zip"
$checksums = Join-Path $dist $(if ($StableNames) { 'SHA256SUMS.txt' } else { "FolderNote-$version-SHA256.txt" })
Compress-Archive -Path "$out\*" -DestinationPath $universalZip -Force
& "$PSScriptRoot\build-win11.ps1" -StableName:$StableNames

Add-Type -AssemblyName System.IO.Compression.FileSystem
$required = 'Install.exe', 'Uninstall.exe', 'FolderNote.exe', 'FolderNoteShell.dll', 'FolderNoteNavigation.dll'
if (Test-Path -LiteralPath "$build\Release\Microsoft.WindowsAppRuntime.Bootstrap.dll") {
    $required += 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
}
foreach ($zip in $universalZip,$win11Zip) {
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $files = if ($zip -eq $win11Zip) { $required + 'Install-Win11.exe','FolderNote.msix' } else { $required }
        foreach ($file in $files) {
            $entry = $archive.GetEntry($file)
            if ($null -eq $entry -or $entry.Length -le 0) { throw "Missing $file at the root of $zip." }
        }
    } finally {
        $archive.Dispose()
    }
}

$universalSetup = Join-Path $dist "$base-Setup-x64.exe"
$win11Setup = Join-Path $dist "$base-Setup-Win11-x64.exe"
& "$PSScriptRoot/build-setup.ps1" -Source $out -Destination $universalSetup
& "$PSScriptRoot/build-setup.ps1" -Source "$dist/FolderNote-Win11-$version" -Destination $win11Setup -Win11

Get-FileHash -Algorithm SHA256 -LiteralPath $universalSetup,$win11Setup,$universalZip,$win11Zip |
    ForEach-Object { "$($_.Hash)  $([IO.Path]::GetFileName($_.Path))" } |
    Set-Content -LiteralPath $checksums -Encoding ascii

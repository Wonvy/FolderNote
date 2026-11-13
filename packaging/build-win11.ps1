param([switch]$StableName)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root 'build\msix-stage'
$manifest = Join-Path $root 'build\AppxManifest.xml'
[xml]$package = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8
$version = ([version]$package.Package.Identity.Version).ToString(3)
$binaryVersion = (Get-Item "$root\build\Release\FolderNote.exe").VersionInfo.ProductVersion
if ($binaryVersion -ne $version) { throw 'Package and executable versions differ. Rebuild before packaging.' }
$out = Join-Path $root "dist\FolderNote-Win11-$version"
New-Item -ItemType Directory -Force "$stage\Assets",$out | Out-Null
Copy-Item "$root\build\Release\FolderNote.exe","$root\build\Release\FolderNoteShell.dll" $stage
Copy-Item $manifest $stage
Copy-Item "$root\assets\Logo.png","$root\assets\SmallLogo.png" "$stage\Assets"
$kits=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$make=Get-ChildItem "$kits\bin\*\x64\makeappx.exe" | Sort-Object FullName -Descending | Select-Object -First 1
& $make.FullName pack /d $stage /p "$out\FolderNote.msix" /o
if($LASTEXITCODE -ne 0){throw 'MSIX validation/packaging failed'}
Copy-Item "$root\build\Release\Install-Win11.exe" $out
Copy-Item "$root\build\Release\Uninstall.exe" $out
Copy-Item "$root\build\Release\Install.exe","$root\build\Release\FolderNote.exe","$root\build\Release\FolderNoteShell.dll","$root\build\Release\FolderNoteNavigation.dll" $out
Copy-Item "$PSScriptRoot\README-Win11.md" $out
Copy-Item "$root\README.md","$root\README.en.md" $out
Copy-Item "$root\CHANGELOG.md","$root\CONTRIBUTING.md" $out
Copy-Item -LiteralPath "$root\docs" -Destination $out -Recurse -Force
New-Item -ItemType Directory -Force "$out\packaging" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot\README-Win11.md" -Destination "$out\packaging"
$zip = if ($StableName) { 'FolderNote-win11-test.zip' } else { "FolderNote-$version-win11-test.zip" }
Compress-Archive -Path "$out\*" -DestinationPath "$root\dist\$zip" -Force

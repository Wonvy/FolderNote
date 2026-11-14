param([string]$Manifest = (Join-Path $PSScriptRoot '..\build\AppxManifest.xml'))
$ErrorActionPreference = 'Stop'
[xml]$document = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8
$namespaces = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
$namespaces.AddNamespace('p', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$namespaces.AddNamespace('v', 'http://schemas.microsoft.com/appx/manifest/virtualization/windows10')
$namespaces.AddNamespace('r', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities')
$namespaces.AddNamespace('c', 'http://schemas.microsoft.com/appx/manifest/com/windows10')
$namespaces.AddNamespace('d5', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5')
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
$keys = @($document.SelectNodes('/p:Package/p:Properties/v:RegistryWriteVirtualization/v:ExcludedKeys/v:ExcludedKey', $namespaces) | ForEach-Object { $_.InnerText.Trim() })
$expected = @(
    'HKEY_CURRENT_USER\Software\FolderNote'
    'HKEY_CURRENT_USER\Software\FolderNote\ExplorerEntries\Custom'
    'HKEY_CURRENT_USER\Software\FolderNote\ExplorerEntryChanges'
    'HKEY_CURRENT_USER\Software\FolderNote\ExplorerEntryChanges\ThisPC'
    'HKEY_CURRENT_USER\Software\FolderNote\ExplorerEntryChanges\Navigation'
    'HKEY_CURRENT_USER\Software\FolderNote\ExplorerEntryChanges\Desktop'
    'HKEY_CURRENT_USER\Software\FolderNote\DesktopFolderLinkChanges'
    'HKEY_CURRENT_USER\Software\Classes\DesktopBackground\shell\FolderNote.DesktopGroups.Create'
    'HKEY_CURRENT_USER\Software\Classes\DesktopBackground\shell\FolderNote.DesktopGroups.Create\command'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run'
    'HKEY_CURRENT_USER\Software\Classes\Directory\shell\FolderNote.Edit'
    'HKEY_CURRENT_USER\Software\Classes\Directory\shell\FolderNote.Clear'
    'HKEY_CURRENT_USER\Software\Classes\Directory\shell\FolderNote.Repair'
    'HKEY_CURRENT_USER\Software\Classes\CLSID'
    'HKEY_CURRENT_USER\Software\Classes\Wow6432Node\CLSID'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\MyComputer\NameSpace_36354489'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\HideMyComputerIcons'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel'
    'HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Policies\NonEnum'
)
Require ($keys.Count -eq $expected.Count) 'Only settings and registrations shared with Explorer may bypass registry virtualization.'
foreach ($key in $expected) {
    Require (@($keys | Where-Object { $_ -eq $key }).Count -eq 1) "Shared registry branch missing or duplicated: $key"
}
Require ($document.SelectNodes('/p:Package/p:Capabilities/r:Capability[@Name="unvirtualizedResources"]', $namespaces).Count -eq 1) 'Registry exclusions require unvirtualizedResources.'
Require ($document.SelectNodes('/p:Package/p:Capabilities/r:Capability[@Name="runFullTrust"]', $namespaces).Count -eq 1) 'The desktop application requires runFullTrust.'
Require (($document.DocumentElement.GetAttribute('IgnorableNamespaces') -split '\s+') -contains 'virtualization') 'The virtualization namespace must be declared ignorable.'
Require ($document.SelectNodes('//*[local-name()="RegistryWriteVirtualization" and namespace-uri()!="http://schemas.microsoft.com/appx/manifest/virtualization/windows10"]').Count -eq 0) 'Do not disable virtualization for all HKCU keys.'
$minimum = $document.SelectSingleNode('/p:Package/p:Dependencies/p:TargetDeviceFamily', $namespaces).GetAttribute('MinVersion')
Require ([version]$minimum -ge [version]'10.0.22000.0') 'Selective registry exclusions are required by this Windows 11 package.'
$settings = $document.SelectSingleNode('//d5:ItemType[@Type="Directory"]/d5:Verb[@Id="FolderNoteSettings"]', $namespaces)
Require ($null -eq $settings) 'Settings belongs in Start, not in the folder context menu.'
Require ($document.SelectNodes('//d5:ItemType[@Type="Directory"]/d5:Verb', $namespaces).Count -eq 3) 'Only the three opt-in note commands belong in the context menu.'
$namespaces.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
$application = $document.SelectSingleNode('/p:Package/p:Applications/p:Application', $namespaces)
$visual = $application.SelectSingleNode('uap:VisualElements', $namespaces)
Require ($application.GetAttribute('Executable') -eq 'FolderNote.exe') 'Start must open the settings program.'
Require ($visual.GetAttribute('AppListEntry') -ne 'none') 'Settings must be visible in Start.'
Write-Output 'PASS MSIX shared settings, narrowly scoped registry exclusions, required capabilities, Start entry and opt-in note commands'

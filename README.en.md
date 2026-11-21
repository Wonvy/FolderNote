# FolderNote

[简体中文](README.md) · [English](README.en.md)

**Windows File Explorer tools · Latest release 1.9.7 · x64**

FolderNote adds desktop boxes, folder entry management, context menu management, folder notes, double-click navigation, a middle-click folder menu, and tabs for Windows 10. It uses a native Win32 settings window and requires no separate C++ runtime installation.

[User guide (Chinese)](docs/USER_GUIDE.md) · [Development and builds (Chinese)](docs/DEVELOPMENT.md) · [Changelog (Chinese)](CHANGELOG.md) · [Contributing (Chinese)](CONTRIBUTING.md)

## Interface preview

![FolderNote Context Menu Manager in English](docs/previews/settings-en-US.png)

| Folder entries | Feature settings |
| --- | --- |
| ![This PC grouped icon view in Simplified Chinese](docs/previews/folder-entries-zh-CN.png) | ![Feature settings in Simplified Chinese](docs/previews/features-zh-CN.png) |
| **Pins and history** | **Custom command editor** |
| ![Pins and history in Simplified Chinese](docs/previews/quick-folders-zh-CN.png) | ![Custom command editor in Simplified Chinese](docs/previews/context-editor-zh-CN.png) |

[View Context Menu Manager in Simplified Chinese](docs/previews/settings-zh-CN.png)

## Features

| Feature | How to use it | Default |
| --- | --- | --- |
| Context Menu Manager | Browse existing menus by Explorer object, hide or restore supported entries, and create ordered FolderNote commands. | Off |
| Folder entries | Add folders in This PC, the navigation pane, or the desktop. Hide, show, or restore supported entries, and remove or recover supported This PC entries. | Available in Settings |
| Desktop boxes | Add boxes to the Windows desktop and move items you drop into them to the `FolderNote Boxes` folder on your desktop. | Off |
| Folder notes | Add, edit, clear, or repair notes from a folder's context menu. Show notes in the Comments column in File Explorer. | Off |
| Double-click empty space | Go up one level or configure the gesture to go back. | Go up one level |
| Middle-click folder menu | Open pinned, recent, and frequently used folders on the desktop or in File Explorer. Hover over folders to browse subfolders. You can also enable the menu in file and folder dialogs. | Off |
| Copy or move by middle click | Select files, middle-click, choose Copy or Move, and select a destination. Windows shows the operation's progress. | Available when the menu opens with items selected |
| Tabs on Windows 10 | Create, duplicate, close, and reopen tabs. Scroll to switch tabs, middle-click to close a tab, or drop files on a tab to transfer them to its folder. | Off |
| Open folders in new tabs | Route new folder windows to tabs in an existing window on Windows 10. A tab's context menu also offers Open in New Window. | Off |
| Appearance | Use a light, dark, or system theme with DPI scaling and the folders' own icons. | Follow system |
| Interface language | Choose Simplified Chinese or English, export a translation template, or import a custom XML language file. | Settings tab |

Windows 11 uses its built-in File Explorer tabs. The Windows 10 option to open folders in tabs opens the first window normally and applies to subsequent folder opens. If an existing window cannot accept the folder, File Explorer opens a separate window.

In a build published on GitHub, choose **Check for updates** at the bottom left of Settings. After you confirm, FolderNote downloads the matching installer package, verifies its SHA-256 hash, and starts the installer.

Enable **Automatic updates** in **Preferences** to check GitHub when you open Settings, up to once a day. The installed standard edition automatically applies updates that require no administrator approval. Reopen Settings to use the new version. For other updates, FolderNote shows the available version so you can select **Check for updates** and confirm installation. Automatic updates are off by default, and changes to this preference take effect immediately.

## Manage context menus

Choose **Context Menu Manager** in the settings sidebar. Browse menus for files, extensions, folders, folder backgrounds, drives, and the desktop, plus the **New**, **Send to**, and **Open with** submenus. Select a row to view its source. The eye icon hides or restores supported entries.

The status bar shows progress while FolderNote reads menus. Some Windows commands are read-only, including **Restore previous versions** on drives.

Add programs, websites, or CMD and PowerShell commands under **Custom commands**. You can limit commands to a folder or file extension and set their order in the FolderNote submenu. The editor can open the Windows folder and icon pickers. See the [user guide](docs/USER_GUIDE.md#右键菜单管理) for supported changes and how to restore them.

## Manage folder entries

Select **Folder entries** and choose a location. Switch between list and icon views, collapse any of the three **This PC** groups, or select an entry to view its status, source, and target path where available. Select **Add folder…** to add your own folder to the selected location. See the [user guide](docs/USER_GUIDE.md#文件夹入口管理) for the hide, restore, and remove actions.

## Organize the desktop

1. In **Features**, enable **Desktop boxes** and save. Windows continues to manage desktop icons, their positions, and the native context menu. Use **Box appearance** to preview and adjust the corner radius, background opacity, frosted glass, and drop shadow.
2. Right-click an empty area of the desktop and select **New box**. On Windows 11, this command may be under **Show more options**. You can also open the creation menu from **Manage desktop boxes** in settings. To configure a box, right-click its title bar.
3. Drag a desktop file or folder into a regular box to move it to the `FolderNote Boxes` folder on your desktop. Drag it back to an empty area of the desktop or right-click and select **Remove from box** to move it back.
4. Right-click empty space inside a box to switch between **Icons** and **List**, change the sort order, or adjust **Item spacing**. Use the button on the right side of the title bar to collapse or expand the box. The title menu lets you map a folder, adjust frosted glass and background opacity, and independently choose **Show icon** and **Show text**.
5. Enable **Auto-collapse** in a box's title menu to expand it when you hover over the title and collapse it shortly after the pointer leaves. This option is off by default.
6. Deleting a box moves its stored items back to your desktop. References from earlier versions still point to their original locations. After turning off desktop boxes or uninstalling FolderNote, you can find stored items by opening `FolderNote Boxes` in File Explorer.

## Install and get started

The development build adds a Chinese-initial candidate popup: `sjsc` finds “设计素材” and `cq` finds “重庆”. The popup appears beside the pointer. Click a candidate, or select it with the arrow keys and press Enter: folders open, while files are only selected and revealed in the original list. Desktop boxes and FolderNote lists support it directly, including box pagination. Enable **Settings → Find items by Chinese initials** to extend it to File Explorer and standard Open, Save, and folder pickers. Matching is offline and supports traditional characters, multiple readings, and mixed Latin names.

1. Download and run **FolderNote-Setup-x64.exe** from [Releases](https://github.com/Wonvy/FolderNote/releases/latest). No extraction is needed. Use **FolderNote-Setup-Win11-x64.exe** for the Windows 11 first-level context menu test edition. Both installers use the application icon and include all program components. The first installation may request administrator approval to register the navigation extension.
2. Find **FolderNote** in the Start menu or open `FolderNote.exe`. Use the sidebar to switch between Features, Pins and history, Context Menu Manager, Folder entries, and Settings. The running version appears at the bottom left.
3. Enable the features you want and save. To display notes, add the **Comments** column to File Explorer's Details view.
4. Close and reopen File Explorer windows after installing or upgrading. If Explorer still has the old extension loaded, wait for any file operations to finish before restarting Explorer.

The standard edition uses the folder context menu on Windows 10 and **Show more options** on Windows 11. The [Windows 11 test edition](packaging/README-Win11.md) uses the newer context menu. Windows determines where it appears.

## Translate the interface

1. Open **FolderNote Settings**, select the **Settings** tab and choose English or Simplified Chinese from the **Language** list.
2. To create another language, click **Export language template** or [download the XML template](docs/FolderNote-language.xml). Edit the XML's `name` attribute and translated strings in a text editor. Keep entry IDs and placeholders such as `{0}` intact. Missing entries use English.
3. Save the file as UTF-8 with an `.xml` extension and click **Import language file** to apply the translation.
4. Close and reopen File Explorer windows to see the localized context menus.

## Data and background processes

- Folder notes are stored in each folder's `desktop.ini`. Keep that file when copying folders. Repair can restore missing attributes. To restore deleted note contents, use a backup of the file.
- Notes, visit history, and desktop group layouts stay on your PC. Manual checks and enabled automatic updates connect to GitHub.
- When desktop boxes are enabled, the desktop helper starts at sign-in without a notification area icon. Layouts are saved to `%LOCALAPPDATA%\Programs\FolderNote\DesktopGroups.json`.
- The Explorer extension handles double-click navigation and Windows 10 tabs. A background helper supports the desktop middle-click menu and opening folders in new tabs. The helper exits when both features are off. Support for file and folder dialogs can be switched off separately.
- Upgrades preserve settings, pinned folders, history, and box layouts. Remove **FolderNote System Tools** through Windows **Settings → Apps**, or run `Uninstall.exe` from its installed version directory. User files, notes, preferences, and history are retained.

## Build

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK, and CMake 3.24 or later.

Desktop boxes use C++ and WinUI 3 by default. Install Microsoft Windows App Runtime 1.8 from the [Windows App SDK downloads archive](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads-archive) to use that interface. Desktop boxes use the Win32 interface when the runtime is unavailable.

```powershell
cmake --preset windows-x64
cmake --build --preset release --parallel
ctest --preset release
```

Binaries go to `build/Release/`. Run `packaging/build-release.ps1` after the Release build to create the installer archives. See the [development guide](docs/DEVELOPMENT.md) for release automation and testing.

| Path | Contents |
| --- | --- |
| `src/` | Native app, Shell extensions, navigation, and interface |
| `assets/` | Icons and other app resources |
| `packaging/` | MSIX manifest, package scripts, and Windows 11 installation notes |
| `tests/` | Automated tests and diagnostic utilities |
| `docs/` | User guide, development guide, and interface previews |
| `CMakeLists.txt` | Build targets and app version |
| `CMakePresets.json` | Windows x64 build and test presets |

## Compatibility

FolderNote targets Windows 10 and 11 on x64. Version 1.9.7 passed 34 CTest tests on Windows 10 22H2, covering desktop boxes, Seer preview layering, dialog history, installation and removal, update selection, and existing features. Both standalone installers also passed file and icon integrity checks. The unsigned Windows 11 first-level context menu package is intended for testing. See the [user guide](docs/USER_GUIDE.md) for supported configurations and troubleshooting.

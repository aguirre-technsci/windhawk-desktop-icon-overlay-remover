# windhawk-desktop-icon-overlay-remover

Windhawk mod: removes the shortcut arrow and the OneDrive sync overlays from desktop icons only. File Explorer windows and Open/Save dialogs are not affected.

Each removal can be turned on or off in the mod settings.

## Install

1. Install [Windhawk](https://windhawk.net/).
2. In Windhawk, click **Create a new mod**.
3. Replace the sample code with the contents of [`desktop-icon-overlay-remover.wh.cpp`](desktop-icon-overlay-remover.wh.cpp).
4. Compile with the button on the left or with `Ctrl+B`.

The mod targets `explorer.exe`. When it is disabled, the desktop is repainted and the overlays come back.

## How it works

The desktop is a `SHELLDLL_DefView` child of `Progman` or `WorkerW`. The mod subclasses only that window and, after the original `LVN_GETDISPINFOW` handling runs, clears the overlay bits (`LVIS_OVERLAYMASK`) when the overlay index is one of the selected ones:

- **Shortcut arrow:** `SHGetIconOverlayIndexW(NULL, IDO_SHGIOI_LINK)`.
- **OneDrive:** handlers registered under `ShellIconOverlayIdentifiers` whose name contains `OneDrive` or `SkyDrive` are queried with `IShellIconOverlayIdentifier::GetOverlayInfo` and converted to the system overlay index.

Overlays from other programs are left alone. Nothing is written to the registry.

## Notes

- With very large desktop icons (Ctrl + mouse wheel), Windows may draw the overlay through another path and it can still show up.
- The OneDrive handlers are read when the mod loads. If OneDrive is installed or updated later, disable and re-enable the mod.

## License

MIT, see [LICENSE](LICENSE).

## Credits

[aguirre-technsci](https://github.com/aguirre-technsci), co-authored with Claude (Anthropic).

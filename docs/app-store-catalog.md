# App Store catalog

NovaOS reads its app list from JSON. Updating the list does not require updating
or restarting the OS once a build with the JSON Store is installed.

Edit `userland/store/catalog.json` through a pull request. After the JSON reaches
`main`, users select **Refresh** in the Store, press **F5**, or run:

```text
store refresh
```

Refresh downloads the current catalog over HTTPS, validates it, and saves it to
`C:\Windows\AppStore\catalog.json`. The Store replaces its displayed list and
clears per-row status messages after the save succeeds. Installed programs and
downloads are detected from their files as before. Removed entries disappear
from the Store but their downloaded or installed files remain on disk.

Opening the Store loads the saved file first. If it is missing or invalid, the
Store loads `C:\Windows\AppStore\default-catalog.json`, bundled with the OS.
To update a disconnected machine, copy a valid catalog into
`C:\Windows\AppStore\catalog.json` and close/reopen the Store. The bundled
fallback is kept separate from the independently maintained catalog.

The default refresh endpoint is:
`https://raw.githubusercontent.com/dean-plude/os/main/userland/store/catalog.json`.
To use your own source, put a complete HTTPS URL on one line in
`C:\Windows\AppStore\catalog-url.txt`. Remove that file to restore the default.
Only use a source you trust: its URLs and install instructions determine which
packages the Store downloads and installs. HTTPS uses NovaOS's existing
certificate validation; catalogs do not currently have a separate signature.

## Format

```json
{
  "schema_version": 1,
  "apps": [
    {
      "name": "Example Editor",
      "publisher": "Example Project",
      "summary": "A portable text editor",
      "category": "developer",
      "url": "https://example.org/releases/1.0/editor.zip",
      "file": "editor-1.0.zip",
      "dest": "Example Editor",
      "exe": "Example Editor\\editor.exe",
      "kind": "archive",
      "size_mb": 12,
      "note": "Compatibility testing pending",
      "label": "Ed",
      "color": "#3671A6"
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `schema_version` | Required integer `1`. |
| `apps` | Required array, at most 256 entries; an empty list is valid. |
| `name`, `publisher`, `summary` | Required display strings, at most 80, 100, and 320 UTF-8 bytes respectively. |
| `category` | `utilities`, `internet`, `media`, `graphics`, `office`, `developer`, or `runtimes`. |
| `url` | Required HTTPS download URL, at most 500 bytes. |
| `file` | Required unique filename under `C:\Downloads`, at most 180 bytes. |
| `kind` | `setup` runs an EXE/MSI installer; `portable` runs the downloaded executable; `archive` uses installed 7-Zip. |
| `dest` | Required for archives: a single directory name under `C:\Programs`. Optional/null otherwise. |
| `exe` | Optional/null installed executable path, relative to `C:\Programs` or beginning with `\` for a root-relative path. Existing `**` folder matching is supported. |
| `size_mb` | Required estimated download size: integer 0–4096. Downloads still obey the network subsystem's actual size limit. |
| `note`, `label`, `color` | Required compatibility note (512 bytes), tile label (4 bytes), and `#RRGGBB` tile color. |
| `system` | Optional archive-only list of files to install in System32/SysWOW64. Existing `x64\file.dll>renamed.dll` syntax is supported. |

The complete document must fit in 512 KiB. Display strings must be nonempty and
contain no control characters. Names and download filenames must be unique
(case insensitive). Install paths cannot traverse `..` or inject quoted command
arguments. `dest` and `file` must be single components. System-file selectors
are restricted to safe relative archive paths and optional single-component
renames. Unknown metadata fields are skipped; changing required behavior needs
a new schema version and corresponding OS support.

A refresh fails safely on invalid JSON, an unsupported version, download errors,
unsafe paths, or write/rename failures. The current list remains available.
Refresh waits for any app download/unpack to finish, and new app actions wait
until refresh finishes. The **Updates** category continues to update NovaOS;
catalog Refresh is a separate operation.

Run the native host tests before publishing changes:

```bash
python3 -m unittest discover -s tests/tools -p test_store_catalog.py -v
```

The first test parses the repository catalog using the actual native reader.
It rejects a catalog that would fail to load in NovaOS. Full install/HTTPS tests
still run through the boot-test suites in CI.

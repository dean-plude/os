## Firefox bundled as the default browser

The image includes Mozilla Firefox 157.0 for Windows x64 at
`C:\Programs\Mozilla Firefox\core\firefox.exe`, the same path used by the
App Store. The build downloads the pinned official full installer and its
Mozilla SHA512SUMS manifest over HTTPS, verifies the installer, and embeds
its unmodified core files. Provenance is kept in the build directory.
No custom browser policies, test CAs, extensions or preference changes are
added to the shipped payload. NetSurf remains installed for its regressions
and as a fallback when Firefox is missing.

HTTP/HTTPS links and local HTML files opened through ShellExecute launch
Firefox. Desktop, Start, dock and Aurora browser actions select Firefox.
Program shortcuts retain their arguments. Disk-image sizing expands beyond
the original 128 MiB minimum when the payload requires it. Builds need 7z;
CI installs p7zip-full. Download/extraction and routing regressions run on
the host; native startup and page rendering still require QEMU validation.
Firefox compatibility failures in the app corpus remain separate runtime
work; bundling does not claim that every site or browser feature works.

- Firefox (tested with Floorp): the browser window opens and draws
  through its GPU process, and its sandboxed child processes start; see
  [Firefox](HISTORY.md#firefox-floorp), and it loads and shows web pages
  over HTTP; it completes TLS handshakes for HTTPS, scrolls and takes
  typing in forms.  Stock Firefox 157 now installs from the App Store
  and loads an HTTPS page (a test CA trusted through `policies.json`) in
  the nightly corpus.  Still open: a page from a publicly trusted HTTPS
  site (the test network has no internet).

# msxmltest: MSXML 6 (msxml6.dll on libxml2) as installers use it.  The
# DOMDocument, XMLHTTP and SAXXMLReader classes and ProgIDs of MSXML 3 and 6
# are registered; a WiX Burn manifest loads and answers XPath with
# SelectionNamespaces; a document built node by node (Edge Update's
# request) serializes as MSXML writes it; parse errors, save/load through a
# file and a file:// URL, UTF-16 and windows-1252 input, bin.base64 values,
# the 3.0/6.0 defaults, whitespace handling and SAX2 callbacks work.
DOC = '`msxmltest` (MSXML 6 DOM, XPath, SAX2 and registration, 64- and 32-bit)'

TESTS = [
    Test('msxml x64', 'msxmltest', [r'msxmltest: \d+ passed, 0 failed']),
    Test('msxml x86', r'C:\Programs\x86\msxmltest.exe', [r'msxmltest: \d+ passed, 0 failed']),
]

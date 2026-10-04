# dltest: a long download over eight connections at once while files are
# written and mapped, as an installer or a game store does; the transfer
# must never stop.  (The kernel's lock waiters once starved a lock's holder
# under this load, and the whole machine stopped part way: kernel/um/um.c.)
DOC = '`dltest -w` (eight connections each downloading 32 MB at once from the host through select(), every byte checked and written to a file while two threads map files over and over; fails if the transfer stops for 5 s)'
TESTS = [
    Test('dltest', 'dltest -n 8 -t 5 -w 10.0.2.2 18080 33554432', [r'dltest: PASS'], timeout=600),
]

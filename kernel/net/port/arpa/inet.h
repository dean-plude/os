/* Empty shim: keeps Mbed TLS from pulling in the host C library's socket
 * headers (x509_crt.c then uses its own inet_pton for IP-address SANs). */
#pragma once

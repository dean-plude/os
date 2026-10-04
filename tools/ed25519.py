"""Ed25519 signatures (RFC 8032) in pure Python, for signing NovaOS's
update channel (tools/mkupdate.py, docs/updates.md).

Adapted from the reference implementation in RFC 8032 section 6 (code
components of IETF documents are under the Simplified BSD License, see
https://trustee.ietf.org/license-info).  It is slow (milliseconds per
operation) and not constant-time, which is fine for signing a few files
on a build machine; the kernel checks signatures with Monocypher
(third_party/monocypher).

A key file holds the 32-byte secret seed as 64 hex digits on a line of
its own; lines starting with '#' are comments.
"""
import hashlib, os

p = 2 ** 255 - 19
q = 2 ** 252 + 27742317777372353535851937790883648493
d = -121665 * pow(121666, p - 2, p) % p
SQRT_M1 = pow(2, (p - 1) // 4, p)


def _sha512(data):
    return hashlib.sha512(data).digest()


def _sha512_modq(data):
    return int.from_bytes(_sha512(data), 'little') % q


def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F, G * H, F * G, E * H)


def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _equal(P, Q):
    return (P[0] * Q[2] - Q[0] * P[2]) % p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % p == 0


def _recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y * y - 1) * pow(d * y * y + 1, p - 2, p)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p:
        x = x * SQRT_M1 % p
    if (x * x - x2) % p:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


_GY = 4 * pow(5, p - 2, p) % p
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % p)


def _compress(P):
    zinv = pow(P[2], p - 2, p)
    x, y = P[0] * zinv % p, P[1] * zinv % p
    return (y | (x & 1) << 255).to_bytes(32, 'little')


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, 'little')
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % p)


def _expand(secret):
    if len(secret) != 32:
        raise ValueError('an Ed25519 secret key is 32 bytes')
    h = _sha512(secret)
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(secret):
    """The 32-byte public key of a 32-byte secret seed"""
    return _compress(_mul(_expand(secret)[0], _G))


def sign(secret, msg):
    """The 64-byte signature of @msg"""
    a, prefix = _expand(secret)
    A = _compress(_mul(a, _G))
    r = _sha512_modq(prefix + msg)
    R = _compress(_mul(r, _G))
    s = (r + _sha512_modq(R + A + msg) * a) % q
    return R + s.to_bytes(32, 'little')


def verify(public, msg, signature):
    """Whether @signature is @public's signature of @msg"""
    if len(public) != 32 or len(signature) != 64:
        return False
    A = _decompress(public)
    R = _decompress(signature[:32])
    if A is None or R is None:
        return False
    s = int.from_bytes(signature[32:], 'little')
    if s >= q:
        return False
    h = _sha512_modq(signature[:32] + public + msg)
    return _equal(_mul(s, _G), _add(R, _mul(h, A)))


def new_secret():
    return os.urandom(32)


def parse_secret(text):
    """The seed in a key file's text (or a secret's value)"""
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            seed = bytes.fromhex(line)
            if len(seed) != 32:
                raise ValueError('an update signing key is 64 hex digits')
            return seed
    raise ValueError('no key in the key file')

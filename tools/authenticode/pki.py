"""DER encoding, RSA and X.509 certificates in plain Python, for the
Authenticode test signatures (tools/authenticode/mktests.py).

Keys come from a seeded generator, so the same seed gives the same keys and
certificates on every build (the test files are reproducible).  The keys
are test keys: their private halves are public in effect, and nothing they
sign is trusted unless a test adds their root to the store itself.
"""
import hashlib
import random

# ---------------------------------------------------------------- DER


def length(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def tlv(tag, body):
    return bytes([tag]) + length(len(body)) + body


def seq(*items):
    return tlv(0x30, b''.join(items))


def set_of(*items):
    """SET OF, its members in DER's sorted order"""
    return tlv(0x31, b''.join(sorted(items)))


def integer(n):
    b = n.to_bytes(max(1, (n.bit_length() + 8) // 8), 'big', signed=True)
    return tlv(0x02, b)


def oid(text):
    parts = [int(x) for x in text.split('.')]
    body = bytearray()
    for v in [parts[0] * 40 + parts[1]] + parts[2:]:
        chunk = [v & 0x7F]
        v >>= 7
        while v:
            chunk.append(0x80 | (v & 0x7F))
            v >>= 7
        body += bytes(reversed(chunk))
    return tlv(0x06, bytes(body))


def null():
    return b'\x05\x00'


def octets(b):
    return tlv(0x04, b)


def bits(b, unused=0):
    return tlv(0x03, bytes([unused]) + b)


def utf8(s):
    return tlv(0x0C, s.encode())


def boolean(v):
    return tlv(0x01, b'\xff' if v else b'\x00')


def explicit(n, body):
    """[n] EXPLICIT (context-specific, constructed)"""
    return tlv(0xA0 | n, body)


def time(when):
    """UTCTime before 2050, GeneralizedTime from then on (RFC 5280);
    @when is (year, month, day, hour, minute, second)"""
    y, mo, d, h, mi, s = when
    if y < 2050:
        return tlv(0x17, f'{y % 100:02d}{mo:02d}{d:02d}{h:02d}{mi:02d}{s:02d}Z'.encode())
    return tlv(0x18, f'{y:04d}{mo:02d}{d:02d}{h:02d}{mi:02d}{s:02d}Z'.encode())


def gentime(when):
    y, mo, d, h, mi, s = when
    return tlv(0x18, f'{y:04d}{mo:02d}{d:02d}{h:02d}{mi:02d}{s:02d}Z'.encode())


def split(der):
    """(tag, header length, value) of the TLV at the start of @der"""
    n = der[1]
    if n < 0x80:
        return der[0], 2, der[2:2 + n]
    k = n & 0x7F
    n = int.from_bytes(der[2:2 + k], 'big')
    return der[0], 2 + k, der[2 + k:2 + k + n]


# ---------------------------------------------------------------- hashes

HASHES = {
    'sha1': ('1.3.14.3.2.26', hashlib.sha1),
    'sha256': ('2.16.840.1.101.3.4.2.1', hashlib.sha256),
    'sha384': ('2.16.840.1.101.3.4.2.2', hashlib.sha384),
    'sha512': ('2.16.840.1.101.3.4.2.3', hashlib.sha512),
}
SIG_OIDS = {'sha1': '1.2.840.113549.1.1.5', 'sha256': '1.2.840.113549.1.1.11',
            'sha384': '1.2.840.113549.1.1.12', 'sha512': '1.2.840.113549.1.1.13'}
RSA_OID = '1.2.840.113549.1.1.1'


def alg(name):
    """a hash's AlgorithmIdentifier"""
    return seq(oid(HASHES[name][0]), null())


def digest(name, data):
    return HASHES[name][1](data).digest()


# ---------------------------------------------------------------- RSA

SMALL_PRIMES = [p for p in range(3, 2000) if all(p % q for q in range(2, int(p ** 0.5) + 1))]


def probable_prime(n, rng):
    if any(n % p == 0 for p in SMALL_PRIMES):
        return n in SMALL_PRIMES
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(32):
        x = pow(rng.randrange(2, n - 2), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def prime(bits, rng):
    while True:
        n = rng.getrandbits(bits) | (3 << (bits - 2)) | 1
        if probable_prime(n, rng):
            return n


class Key:
    """An RSA key from a seed (2048 bits: Mbed TLS's least by default)"""

    def __init__(self, seed, size=2048):
        rng = random.Random(seed)
        e = 65537
        while True:
            p, q = prime(size // 2, rng), prime(size // 2, rng)
            phi = (p - 1) * (q - 1)
            if p != q and phi % e and (p * q).bit_length() == size:
                break
        self.n, self.e, self.d = p * q, e, pow(e, -1, phi)
        self.p, self.q = p, q
        self.dp, self.dq, self.qi = self.d % (p - 1), self.d % (q - 1), pow(q, -1, p)
        self.size = (self.n.bit_length() + 7) // 8

    def public(self):
        """SubjectPublicKeyInfo"""
        return seq(seq(oid(RSA_OID), null()), bits(seq(integer(self.n), integer(self.e))))

    def sign(self, hash_name, data):
        """PKCS #1 v1.5 signature of @data's @hash_name hash"""
        info = seq(alg(hash_name), octets(digest(hash_name, data)))
        em = b'\x00\x01' + b'\xff' * (self.size - len(info) - 3) + b'\x00' + info
        m = int.from_bytes(em, 'big')
        m1, m2 = pow(m, self.dp, self.p), pow(m, self.dq, self.q)
        s = m2 + self.q * (self.qi * (m1 - m2) % self.p)
        return s.to_bytes(self.size, 'big')


# ---------------------------------------------------------------- X.509

EKU = {'codesign': '1.3.6.1.5.5.7.3.3', 'timestamp': '1.3.6.1.5.5.7.3.8', 'server': '1.3.6.1.5.5.7.3.1'}


def name(cn, org='NovaOS'):
    return seq(set_of(seq(oid('2.5.4.6'), tlv(0x13, b'US'))),
               set_of(seq(oid('2.5.4.10'), utf8(org))),
               set_of(seq(oid('2.5.4.3'), utf8(cn))))


def ext(ext_oid, value, critical=False):
    return seq(oid(ext_oid), *([boolean(True)] if critical else []), octets(value))


class Cert:
    """A certificate for @key, signed by @issuer (a Cert) with @issuer_key,
    or self-signed when @issuer is None"""

    def __init__(self, cn, key, serial, valid, issuer=None, issuer_key=None, ca=False, usage=None):
        self.cn, self.key, self.serial = cn, key, serial
        self.name = name(cn)
        skid = hashlib.sha1(seq(integer(key.n), integer(key.e))).digest()      # (of the key's BIT STRING value)
        exts = [ext('2.5.29.19', seq(boolean(True)) if ca else seq(), critical=True),
                ext('2.5.29.15', bits(b'\x06', 1) if ca else bits(b'\x80', 7), critical=True),
                ext('2.5.29.14', octets(skid))]
        if issuer:
            exts.append(ext('2.5.29.35', seq(tlv(0x80, issuer.skid))))
        if usage:
            exts.append(ext('2.5.29.37', seq(oid(EKU[usage])), critical=usage == 'timestamp'))
        self.skid = skid
        signer, signer_key = (issuer.name, issuer_key) if issuer else (self.name, key)
        tbs = seq(explicit(0, integer(2)), integer(serial), seq(oid(SIG_OIDS['sha256']), null()), signer,
                  seq(time(valid[0]), time(valid[1])), self.name, key.public(), explicit(3, seq(*exts)))
        self.der = seq(tbs, seq(oid(SIG_OIDS['sha256']), null()), bits(signer_key.sign('sha256', tbs)))
        self.issuer_name = signer

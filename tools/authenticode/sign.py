"""Authenticode signing of PE files in plain Python (tools/authenticode/
mktests.py signs NovaOS's test files with it).

sign(pe, signer, chain, hash) returns the PE with a PKCS #7 SignedData in
its certificate table, as signtool and osslsigncode write it: the file's
Authenticode digest (everything before the table but the checksum and the
table's directory entry) in an SpcIndirectDataContent, signed with
authenticated attributes.  Optional: a PKCS #9 countersignature or an RFC
3161 token from a time-stamping authority, and nested signatures.
"""
import struct

from pki import (alg, digest, explicit, gentime, integer, null, octets, oid, seq, set_of, split, time, tlv, bits,
                 RSA_OID)

SPC_INDIRECT_DATA = '1.3.6.1.4.1.311.2.1.4'
SPC_PE_IMAGE_DATA = '1.3.6.1.4.1.311.2.1.15'
SIGNED_DATA = '1.2.840.113549.1.7.2'


def layout(pe):
    """(checksum offset, certificate directory entry offset)"""
    e = struct.unpack_from('<I', pe, 60)[0]
    magic = struct.unpack_from('<H', pe, e + 24)[0]
    return e + 24 + 64, e + 24 + (112 if magic == 0x20B else 96) + 32


def pe_digest(pe, hash_name):
    """the Authenticode digest of an unsigned, 8-byte padded PE"""
    cks, dirent = layout(pe)
    return digest(hash_name, pe[:cks] + pe[cks + 4:dirent] + pe[dirent + 8:])


def attr(attr_oid, *values):
    return seq(oid(attr_oid), set_of(*values))


def signer_info(cert, key, hash_name, auth_attrs, unauth=b'', version=1):
    """SignerInfo by @cert over @auth_attrs (a list of attribute encodings)"""
    auth = set_of(*auth_attrs)
    sig = key.sign(hash_name, auth)
    return seq(integer(version), seq(cert.issuer_name, integer(cert.serial)), alg(hash_name),
               b'\xa0' + auth[1:], seq(oid(RSA_OID), null()), octets(sig),
               (b'\xa1' + unauth[1:]) if unauth else b'')


def signed_data(content_type, content, certs, signers, hash_name, version=1):
    """ContentInfo { signedData }; @content is the encoding inside [0]"""
    sd = seq(integer(version), set_of(alg(hash_name)), seq(oid(content_type), explicit(0, content)),
             explicit(0, b''.join(c.der for c in certs)), set_of(*signers))
    return seq(oid(SIGNED_DATA), explicit(0, sd))


def countersignature(tsa, hash_name, outer_sig, when):
    """a PKCS #9 countersignature attribute: the TSA signs the signer's
    encrypted digest at @when"""
    cs = signer_info(tsa, tsa.key, hash_name,
                     [attr('1.2.840.113549.1.9.3', oid('1.2.840.113549.1.7.1')),
                      attr('1.2.840.113549.1.9.5', time(when)),
                      attr('1.2.840.113549.1.9.4', octets(digest(hash_name, outer_sig)))])
    return attr('1.2.840.113549.1.9.6', cs)


def rfc3161(tsa, chain, hash_name, outer_sig, when):
    """an RFC 3161 time-stamp token attribute (Microsoft's
    1.3.6.1.4.1.311.3.3.1) over the signer's encrypted digest"""
    tst = seq(integer(1), oid('1.3.6.1.4.1.311.97.1.0'),
              seq(alg(hash_name), octets(digest(hash_name, outer_sig))), integer(1), gentime(when))
    ct = '1.2.840.113549.1.9.16.1.4'
    si = signer_info(tsa, tsa.key, hash_name,
                     [attr('1.2.840.113549.1.9.3', oid(ct)), attr('1.2.840.113549.1.9.4', octets(digest(hash_name, tst))),
                      attr('1.2.840.113549.1.9.16.2.47', seq(seq(seq(octets(digest('sha256', tsa.der))))))])   # ESS signingCertificateV2
    return attr('1.3.6.1.4.1.311.3.3.1', signed_data(ct, octets(tst), [tsa] + chain, [si], hash_name, version=3))


def make_signature(pe, signer, chain, hash_name, timestamp=None, nested=()):
    """the SignedData for @pe (unsigned, padded): @signer (a Cert with its
    key) and the certificates to send with it; @timestamp is
    (kind, tsa, tsa_chain, when) with kind 'pkcs9' or 'rfc3161'; @nested
    are further SignedData encodings to nest"""
    spc = seq(seq(oid(SPC_PE_IMAGE_DATA), seq(bits(b''), explicit(0, explicit(2, tlv(0x80, b''))))),
              seq(alg(hash_name), octets(pe_digest(pe, hash_name))))
    body = split(spc)[2]                                    # PKCS #7: only the contents octets are hashed
    auth = [attr('1.2.840.113549.1.9.3', oid(SPC_INDIRECT_DATA)),
            attr('1.3.6.1.4.1.311.2.1.12', seq()),
            attr('1.2.840.113549.1.9.4', octets(digest(hash_name, body)))]
    first = signer_info(signer, signer.key, hash_name, auth)
    unauth = []
    certs = [signer] + list(chain)
    if timestamp or nested:
        sig = signer.key.sign(hash_name, set_of(*auth))     # (PKCS #1 v1.5: the same signature again)
        if timestamp:
            kind, tsa, tsa_chain, when = timestamp
            if kind == 'pkcs9':
                unauth.append(countersignature(tsa, hash_name, sig, when))
                certs += [tsa] + [c for c in tsa_chain if c not in certs]
            else:
                unauth.append(rfc3161(tsa, tsa_chain, hash_name, sig, when))
        for n in nested:
            unauth.append(attr('1.3.6.1.4.1.311.2.4.1', n))
        first = signer_info(signer, signer.key, hash_name, auth, set_of(*unauth))
    return signed_data(SPC_INDIRECT_DATA, spc, certs, [first], hash_name)


def pad(pe):
    return pe + b'\0' * (-len(pe) % 8)


def embed(pe, sd):
    """@pe (unsigned, padded) with @sd in its certificate table"""
    cks, dirent = layout(pe)
    blob = sd + b'\0' * (-len(sd) % 8)
    wc = struct.pack('<IHH', 8 + len(blob), 0x0200, 0x0002) + blob
    out = bytearray(pe + wc)
    struct.pack_into('<II', out, dirent, len(pe), len(wc))
    return bytes(out)


def sign(pe, signer, chain, hash_name, timestamp=None, nested=()):
    pe = pad(pe)
    return embed(pe, make_signature(pe, signer, chain, hash_name, timestamp, nested))

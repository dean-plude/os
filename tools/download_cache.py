"""URL identities, retained content hashes, mutable TTL and replayable corpus inputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from urllib.parse import urlsplit, unquote


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix='.metadata-')
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(value, f, indent=2)
            f.write('\n')
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


class DownloadCache:
    def __init__(self, root, mutable_max_age=86400, lock=None, journal=None, clock=time.time):
        if mutable_max_age < 0:
            raise ValueError('mutable max age must be nonnegative')
        self.root = Path(root)
        self.max_age, self.journal, self.clock = mutable_max_age, journal, clock
        self.records = {}
        self.lock = None
        if lock:
            data = json.loads(Path(lock).read_text())
            self.lock = {item['source_url']: item for item in data['downloads']}
            for item in self.lock.values():
                if not re.fullmatch('[0-9a-f]{64}', item['sha256']):
                    raise ValueError('invalid download lock hash')
        (self.root / 'blobs').mkdir(parents=True, exist_ok=True)

    def observe_version(self, url, version, evidence):
        if url in self.records:
            self.records[url] = dict(self.records[url], resolved_version=version,
                                     version_evidence=evidence)
            if self.journal:
                atomic_json(self.journal, dict(schema=1, downloads=list(self.records.values())))

    def fetch(self, url, mutable=False, version=None):
        if not url:
            return None
        identity = hashlib.sha256(url.encode()).hexdigest()
        metadata = self.root / 'urls' / (identity + '.json')
        pinned = self.lock is not None
        if pinned and url not in self.lock:
            raise ValueError(f'download lock has no entry for {url}')
        try:
            record = self.lock[url] if pinned else json.loads(metadata.read_text())
        except (FileNotFoundError, ValueError):
            record = None
        valid = record and record.get('source_url') == url and re.fullmatch('[0-9a-f]{64}', record.get('sha256', ''))
        blob = self.root / 'blobs' / record['sha256'] if valid else None
        fresh = pinned or not mutable or (valid and 0 <= self.clock() - record['fetched_at'] < self.max_age)
        if not (valid and fresh and blob.is_file() and blob.stat().st_size and sha256(blob) == record['sha256']):
            fd, tmp = tempfile.mkstemp(dir=self.root, prefix='.download-')
            os.close(fd)
            try:
                response = subprocess.run(['curl', '-sSLf', '--retry', '4', '--connect-timeout', '30',
                                           '--max-time', '600', '-o', tmp, '-w', '%{url_effective}', url],
                                          check=True, capture_output=True, text=True)
                if not os.path.getsize(tmp):
                    raise ValueError(f'empty download: {url}')
                digest = sha256(tmp)
                if pinned and digest != self.lock[url]['sha256']:
                    raise ValueError(f'locked download hash mismatch: {url}')
                effective = response.stdout.strip() or url
                match = re.search(r'(?<!\d)(\d+\.\d+\.\d+(?:\.\d+)?)(?!\d)', unquote(urlsplit(effective).path))
                record = dict(source_url=url, resolved_url=effective, sha256=digest,
                              fetched_at=self.clock(), mutable=mutable, declared_version=version,
                              resolved_version=match.group(1) if match else None,
                              version_evidence='resolved URL path' if match else None)
                blob = self.root / 'blobs' / digest
                os.replace(tmp, blob)
                atomic_json(metadata, record)
            finally:
                if os.path.exists(tmp):
                    os.unlink(tmp)
        record = dict(record, mutable=mutable, declared_version=version or record.get('declared_version'))
        self.records[url] = record
        if self.journal:
            atomic_json(self.journal, dict(schema=1, downloads=list(self.records.values())))
        # Preserve the URL extension for unpackers that inspect filenames. A URL
        # digest directory prevents query strings and equal basenames colliding.
        name = re.sub(r'[^a-zA-Z0-9._-]', '_', unquote(Path(urlsplit(url).path).name)) or 'download'
        if name in {'.', '..'}:
            name = 'download'
        target = self.root / 'files' / identity / name
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=target.parent, prefix='.link-')
        os.close(fd)
        os.unlink(tmp)
        try:
            os.link(blob, tmp)
            os.replace(tmp, target)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        return str(target)

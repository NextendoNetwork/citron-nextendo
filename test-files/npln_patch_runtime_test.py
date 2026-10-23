"""Exercise the real catalog/cache code against an isolated HTTP fixture server.

Only the fixed host and public key are substituted in a temporary source copy.
Production source and user cache are never changed. Requires g++, OpenSSL, fmt,
nlohmann-json and the cpp-httplib header (or its packaged shared library).
"""
import argparse
import base64
from datetime import datetime, timezone, timedelta
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

parser = argparse.ArgumentParser()
parser.add_argument('--include', action='append', default=[])
parser.add_argument('--library', action='append', default=[])
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
key = Ed25519PrivateKey.generate()
public = base64.b64encode(key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)).decode()
now = datetime.now(timezone.utc)


def signed(payload):
    raw = json.dumps(payload, separators=(',', ':')).encode()
    return json.dumps({'key_id': 'npln-patches-key-1',
                       'payload': base64.b64encode(raw).decode(),
                       'signature': base64.b64encode(key.sign(raw)).decode()}).encode()


metadata = {'schema_version': 1, 'revision': 2,
            'issued_at': now.isoformat().replace('+00:00', 'Z'),
            'expires_at': (now + timedelta(hours=24)).isoformat().replace('+00:00', 'Z')}
title = '0100000000000001'
index_tag = '"' + 'a' * 64 + '"'
patch_tag = '"' + 'b' * 64 + '"'
index = signed(dict(metadata, document_type='npln_title_index', index_etag=index_tag,
                    titles=[{'title_id': title, 'enabled': True}]))
ips = b'IPS32' + (256).to_bytes(4, 'big') + b'\x00\x01\x02EEOF'
patch = signed(dict(metadata, document_type='npln_patch_catalog', patch_etag=patch_tag,
                    patches=[{'id': 'new-title', 'title_id': title, 'build_id': 'A' * 64,
                              'module': 'main', 'enabled': True, 'patch_version': 1,
                              'required_capabilities': ['npln.ips32.v1'],
                              'offset_basis': 'nso_header_included',
                              'ips32_base64': base64.b64encode(ips).decode(),
                              'sha256': hashlib.sha256(ips).hexdigest(),
                              'guards': [{'offset': 256, 'original_hex': '01', 'patched_hex': '02'}]}]))
requests = []
mode = 'normal'


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        requests.append((self.path, self.headers.get('If-None-Match')))
        time.sleep(0.15)
        status = 200
        if self.path.endswith('/npln-title-index'):
            body, tag = index, index_tag
            if mode == 'failed':
                status, body = 503, b''
            elif mode == 'rollback':
                tag = '"' + 'c' * 64 + '"'
                body = signed(dict(metadata, revision=1, document_type='npln_title_index',
                                   index_etag=tag, titles=[]))
            elif mode == 'tampered':
                tag = '"' + 'd' * 64 + '"'
                envelope = json.loads(index)
                envelope['signature'] = 'A' * 88
                body = json.dumps(envelope).encode()
            elif mode == 'tag-mismatch':
                tag = '"' + 'e' * 64 + '"'
            elif mode == 'bad-304':
                status, body, tag = 304, b'', '"' + 'f' * 64 + '"'
        elif self.path.endswith('/npln-patches/' + title + '/' + 'A' * 64):
            body, tag = patch, patch_tag
        else:
            status, body, tag = 404, b'', ''
        if status == 200 and self.headers.get('If-None-Match') == tag:
            status, body = 304, b''
        self.send_response(status)
        self.send_header('ETag', tag)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        requests[-1] += (status, len(body))

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        stub = root / 'common'
        (stub / 'fs').mkdir(parents=True)
        (stub / 'fs' / 'path_util.h').write_text('''#pragma once
#include <cstdlib>
#include <filesystem>
namespace Common::FS {
enum class CitronPath { CacheDir };
inline std::filesystem::path GetCitronPath(CitronPath) { return std::getenv("NPLN_TEST_CACHE"); }
}
''')
        (stub / 'logging.h').write_text('''#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
''')
        source = (repo / 'src/web_service/npln_patch_catalog.cpp').read_text()
        assert source.count('https://nextendo.network') == 2
        source = source.replace('https://nextendo.network', 'http://127.0.0.1:' + str(server.server_port))
        source = source.replace('LAI+Pi+GZtj3p4DpNM5ORBraw1cbZCVSMFgUmAVWNeA=', public)
        (root / 'catalog.cpp').write_text(source)
        command = [os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-pthread',
                   '-DCPPHTTPLIB_OPENSSL_SUPPORT', '-DFMT_HEADER_ONLY', '-I' + str(root),
                   '-I' + str(repo / 'src')]
        command += ['-I' + path for path in args.include]
        command += [str(root / 'catalog.cpp'), str(repo / 'src/web_service/npln_patch_manifest.cpp'),
                    str(repo / 'test-files/npln_patch_runtime_test.cpp'), '-lssl', '-lcrypto']
        command += ['-l' + name for name in args.library]
        command += ['-o', str(root / 'test-runtime')]
        subprocess.run(command, check=True)
        environment = dict(os.environ, NPLN_TEST_CACHE=str(root / 'cache'))
        for scenario in ['normal', 'normal', 'failed', 'rollback', 'tampered', 'tag-mismatch', 'bad-304']:
            mode = scenario
            previous = len(requests)
            subprocess.run([str(root / 'test-runtime')], env=environment, check=True, timeout=30)
            current = requests[previous:]
            indexes = [request for request in current if request[0].endswith('/npln-title-index')]
            patches = [request for request in current if '/npln-patches/' in request[0]]
            assert len(indexes) == 1, current
            if previous == 0:
                assert indexes[0][1] is None and len(patches) == 1
            else:
                assert indexes[0][1] == index_tag and not patches, current
                if scenario == 'normal':
                    assert indexes[0][2:] == (304, 0)
        # Expiration never discards authentic cached data when the API is unavailable.
        expired_metadata = dict(metadata,
                                issued_at=(now - timedelta(hours=25)).isoformat().replace('+00:00', 'Z'),
                                expires_at=(now - timedelta(hours=1)).isoformat().replace('+00:00', 'Z'))
        (root / 'cache/nextendo-npln-patches/title-index/revision-2.json').write_bytes(
            signed(dict(expired_metadata, document_type='npln_title_index', index_etag=index_tag,
                        titles=[{'title_id': title, 'enabled': True}])))
        expired_patch = json.loads(base64.b64decode(json.loads(patch)['payload']))
        expired_patch.update(expired_metadata)
        (root / ('cache/nextendo-npln-patches/verified/' + title + '-' + 'A' * 64 + '-revision-2.json')).write_bytes(signed(expired_patch))
        mode = 'failed'
        previous = len(requests)
        subprocess.run([str(root / 'test-runtime')], env=environment, check=True, timeout=30)
        assert len(requests) == previous + 1 and requests[-1][1] == index_tag
        # A first offline launch cannot invent discovery information for an unseen title.
        environment['NPLN_TEST_CACHE'] = str(root / 'empty-cache')
        previous = len(requests)
        subprocess.run([str(root / 'test-runtime'), 'unknown'], env=environment, check=True, timeout=30)
        assert len(requests) == previous + 1 and requests[-1][1] is None
        print('PASS: launch race, new title/exact patch, one index request per process, restart 304, '
              'persisted 15-minute patch interval, outage/tamper/rollback/tag cache fallback, '
              'expired cache, first offline launch, no loader polling')
finally:
    server.shutdown()
    server.server_close()

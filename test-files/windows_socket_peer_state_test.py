"""Native loopback regression for the actual Windows TCP peer-state probe.

Run in a Windows compiler environment:
    python test-files/windows_socket_peer_state_test.py --cxx clang-cl
or with an LLVM/MinGW toolchain:
    python test-files/windows_socket_peer_state_test.py --cxx clang++

The old implementation blocks on an idle blocking descriptor. The fixture
releases that read after 120 ms so a regression fails instead of hanging CI.
No external network, accounts, game data or emulator process is accessed.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang-cl'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / 'src/core/internal_network/network.cpp').read_text()
    match = re.search(r'bool PeekPeerHungUp\(SOCKET native_fd, bool& has_pending_data\) \{.*?\n\}', source, re.S)
    if not match:
        raise RuntimeError('Peer-state probe not found; update the test extraction')
    fixture = (Path(__file__).with_suffix('.cpp.in')).read_text().replace('PROBE_SOURCE', match[0])
    with tempfile.TemporaryDirectory(prefix='citron-tcp-probe-') as folder:
        test = Path(folder) / 'probe.cpp'
        binary = Path(folder) / 'probe.exe'
        test.write_text(fixture)
        compiler = Path(args.cxx).name.lower()
        if compiler in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
            command = [args.cxx, '/nologo', '/EHsc', '/std:c++20', str(test), '/Fe:' + str(binary), '/link', 'ws2_32.lib']
        else:
            command = [args.cxx, '-std=c++20', str(test), '-lws2_32', '-o', str(binary)]
        subprocess.run(command, check=True, cwd=folder, timeout=90)
        subprocess.run([str(binary)], check=True, cwd=folder, timeout=15)


if __name__ == '__main__':
    main()

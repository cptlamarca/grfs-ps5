#!/usr/bin/env python3
"""FTP helper for the PS5 (ftpsrv: PASV only, LIST but no NLST/MLSD).

usage: ps5_ftp.py HOST:PORT ls REMOTE_DIR          (recursive: size path)
       ps5_ftp.py HOST:PORT put LOCAL REMOTE        (file or directory tree)
"""
import ftplib, os, sys


def connect(hostport):
    host, port = hostport.split(':')
    ftp = ftplib.FTP()
    ftp.connect(host, int(port), timeout=60)
    ftp.login()
    ftp.voidcmd('TYPE I')
    return ftp


def entries(ftp, path):
    lines = []
    ftp.retrlines('LIST ' + path, lines.append)
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9 or parts[8] in ('.', '..'):
            continue
        yield parts[0].startswith('d'), int(parts[4]), parts[8]


def walk(ftp, path):
    for is_dir, size, name in entries(ftp, path):
        full = path.rstrip('/') + '/' + name
        if is_dir:
            yield from walk(ftp, full)
        else:
            yield size, full


def mkdirs(ftp, path):
    parts = path.strip('/').split('/')
    for i in range(1, len(parts) + 1):
        try:
            ftp.mkd('/' + '/'.join(parts[:i]))
        except ftplib.error_perm:
            pass


def put(ftp, local, remote):
    if os.path.isdir(local):
        mkdirs(ftp, remote)
        for name in sorted(os.listdir(local)):
            put(ftp, os.path.join(local, name), remote.rstrip('/') + '/' + name)
        return
    with open(local, 'rb') as f:
        ftp.storbinary('STOR ' + remote, f, blocksize=1 << 20)
    print(os.path.getsize(local), remote, flush=True)


def main():
    ftp = connect(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'ls':
        for size, path in walk(ftp, sys.argv[3]):
            print(size, path)
    elif cmd == 'put':
        put(ftp, sys.argv[3], sys.argv[4])
    ftp.quit()


main()

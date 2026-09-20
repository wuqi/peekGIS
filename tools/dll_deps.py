#!/usr/bin/env python3
"""计算 Windows 构建产物真正需要的运行时 DLL(导入闭包), 用于维护 xmake.lua 里的
`runtime_dlls` 白名单(不再全量拷贝 vcpkg/bin)。

原理: 从 bin 下所有 .exe 出发, 用 dumpbin /imports 递归展开它们导入的、且存在于
同目录的 DLL。不在闭包里的 DLL 即"没人导入", 可安全删除(除非某个驱动在运行期用
LoadLibrary 动态加载, 那类需按需保留)。

用法:
  python tools/dll_deps.py [--bin DIR] [--move DIR] [--dumpbin PATH]
    --bin    构建产物目录(默认 build/windows/x64/release/bin)
    --move   把"不需要"的 DLL 移到该目录(默认只列出, 不移动)
    --dumpbin 指定 dumpbin.exe; 默认用 vswhere 定位 VS, 或 PATH
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import deque

sys.stdout.reconfigure(encoding='utf-8')


def find_dumpbin(explicit):
    if explicit:
        return explicit
    if os.environ.get('DUMPBIN') and os.path.exists(os.environ['DUMPBIN']):
        return os.environ['DUMPBIN']
    vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                           r'Microsoft Visual Studio\Installer\vswhere.exe')
    if os.path.exists(vswhere):
        try:
            vs = subprocess.run([vswhere, '-latest', '-property', 'installationPath'],
                                capture_output=True, text=True).stdout.strip()
            tools = os.path.join(vs, r'VC\Tools\MSVC')
            if os.path.isdir(tools):
                for ver in sorted(os.listdir(tools), reverse=True):
                    db = os.path.join(tools, ver, r'bin\Hostx64\x64\dumpbin.exe')
                    if os.path.exists(db):
                        return db
        except Exception:
            pass
    return 'dumpbin'   # 交给 PATH


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default=r'build/windows/x64/release/bin')
    ap.add_argument('--move', default=None)
    ap.add_argument('--dumpbin', default=None)
    args = ap.parse_args()

    dumpbin = find_dumpbin(args.dumpbin)
    files = [f for f in os.listdir(args.bin) if f.lower().endswith(('.exe', '.dll'))]
    actual = {f.lower(): f for f in files}
    present = set(actual)
    cache = {}

    def deps(path):
        key = path.lower()
        if key in cache:
            return cache[key]
        out = subprocess.run([dumpbin, '/nologo', '/imports', path],
                             capture_output=True, text=True, errors='ignore').stdout
        ds = set()
        for line in out.splitlines():
            m = re.match(r'^\s+([A-Za-z0-9_\-\.]+\.dll)\s*$', line)
            if m:
                ds.add(m.group(1).lower())
        cache[key] = ds
        return ds

    seen = set()
    q = deque()
    for f in files:
        if f.lower().endswith('.exe'):
            seen.add(f.lower())
            q.append(f.lower())
    while q:
        cur = q.popleft()
        for d in deps(os.path.join(args.bin, actual[cur])):
            if d in present and d not in seen:
                seen.add(d)
                q.append(d)

    need = sorted(actual[s] for s in seen if s.endswith('.dll'))
    all_dlls = sorted(f for f in files if f.lower().endswith('.dll'))
    removable = [f for f in all_dlls if f.lower() not in seen]

    def mb(lst):
        return sum(os.path.getsize(os.path.join(args.bin, f)) for f in lst) / 1048576.0

    print('需要 %d 个 dll (%.1f MB):' % (len(need), mb(need)))
    for f in need:
        print('    "%s",' % f)
    print()
    print('不需要 %d 个 dll (%.1f MB)' % (len(removable), mb(removable)))
    if args.move:
        os.makedirs(args.move, exist_ok=True)
        for f in removable:
            shutil.move(os.path.join(args.bin, f), os.path.join(args.move, f))
        print('已移动到: %s' % args.move)
    else:
        for f in removable:
            print('  ' + f)


if __name__ == '__main__':
    main()

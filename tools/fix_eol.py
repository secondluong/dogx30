#!/usr/bin/env python3
"""把脚本里的 CRLF 换成 LF。

在 Windows 上编辑、到 Linux 上执行的项目里，这是个反复出现的问题：
带 CR 的 shell 脚本在 Linux 下会报 "command not found" 或
"unexpected end of file"，而报错信息完全看不出是行尾的锅。

    python3 tools/fix_eol.py            # 修全部脚本
    python3 tools/fix_eol.py a.sh b.py  # 只修指定文件

注意别用 `tr -d '\\r'` 从 PowerShell 调 wsl 来做这件事 ——
参数里的反斜杠会被吃掉，结果是把文件里所有字母 r 删光。这个脚本没有这个坑。
"""

import os
import sys

# 不只是脚本。systemd 单元带 CR 的后果尤其隐蔽：反斜杠续行会断，
# 或者 \r 被当成参数的一部分传进程序（--ros-host "192.168.1.120\r"）。
# 这个坑真踩过一次，是靠 install_dryrun 的测试才抓出来的。
PATTERNS = ('.sh', '.py', '.js', '.service', '.yml', '.yaml',
            '.json', '.toml', '.conf')
SKIP_DIRS = {'.git', 'build', 'build-wsl', 'dist', 'node_modules'}

CR = bytes([13])
LF = bytes([10])


def fix(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        print('读不了 %s: %s' % (path, e))
        return False
    if CR not in data:
        return False
    # 只动行尾的 CR，不碰二进制内容里可能出现的孤立 CR
    fixed = data.replace(CR + LF, LF)
    if fixed == data:
        return False
    with open(path, 'wb') as f:
        f.write(fixed)
    print('已修 %s' % path)
    return True


def walk(root):
    out = []
    for base, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            if name.endswith(PATTERNS):
                out.append(os.path.join(base, name))
    return out


def main():
    if len(sys.argv) > 1:
        targets = sys.argv[1:]
    else:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        targets = walk(root)

    n = sum(1 for p in targets if fix(p))
    print('共修改 %d 个文件' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())

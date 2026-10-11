"""Allocate the next MSI build from remote tag history; never reserve a live tag."""
import re
import subprocess
from pathlib import Path


def next_version(tags):
    maximum = 0
    for tag in tags:
        match = re.fullmatch(r"v6\.1\.0-vrr(\d+)(?:\.(\d+))?", tag)
        if match:
            n, patch = match.groups()
            n = int(n)
            build = n * 10 + int(patch) if patch is not None else (n if n <= 17 else n * 10)
            maximum = max(maximum, build)
            continue
        match = re.fullmatch(r"v6\.1\.0-daily\.(\d+)", tag)
        if match:
            maximum = max(maximum, int(match[1]))
            continue
        # A newer stable or unfamiliar custom release needs an explicit migration.
        match = re.match(r"v?(\d+)\.(\d+)\.(\d+)(?:$|-)", tag)
        if match and tuple(map(int, match.groups())) > (6, 1, 0):
            raise ValueError(f"Review Windows version allocation for newer release tag {tag}")
        if tag.startswith('v6.1.0-daily') or tag.startswith('v6.1.0-vrr'):
            raise ValueError(f"Unrecognized Windows release tag {tag}")
    build = maximum + 1
    if build > 65535:
        raise ValueError('Windows MSI build field exhausted; explicit version migration required')
    return f'6.1.0-daily.{build}'


if __name__ == '__main__':
    import os
    tags = subprocess.check_output(['git', 'tag', '--list'], text=True).splitlines()
    version = next_version(tags)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f'ci_version={version}\ntag=v{version}\n')
    print(f'Allocated {version} (Windows 6.2.{version.rsplit(".", 1)[1]})')

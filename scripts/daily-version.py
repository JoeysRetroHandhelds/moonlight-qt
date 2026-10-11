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
        if match and '-' in tag and tuple(map(int, match.groups())) >= (6, 1, 0):
            raise ValueError(f"Unrecognized Windows release tag {tag}")
    build = maximum + 1
    if build > 65535:
        raise ValueError('Windows MSI build field exhausted; explicit version migration required')
    return f'6.1.0-daily.{build}'


def verify_draft(release, source_sha, tag, remote_tag_sha=None):
    if (release.get('isDraft') is not True or release.get('isPrerelease') is not True
            or release.get('targetCommitish') != source_sha or release.get('tagName') != tag):
        raise ValueError('Draft release state or immutable target changed')
    if remote_tag_sha is not None and remote_tag_sha != source_sha:
        raise ValueError('Existing remote release tag targets a different commit')


def check_vrr_version(ci_version, tags):
    match = re.fullmatch(r'6\.1\.0-vrr(\d+)(?:\.(\d+))?', ci_version)
    if not match:
        raise ValueError('Unsupported VRR version family')
    n, patch = match.groups()
    n = int(n)
    build = n * 10 + int(patch) if patch is not None else (n if n <= 17 else n * 10)
    for tag in tags:
        if tag.startswith('v6.1.0-daily'):
            daily = re.fullmatch(r'v6\.1\.0-daily\.(\d+)', tag)
            if not daily or int(daily[1]) >= build:
                raise ValueError(f'VRR Windows 6.2.{build} must exceed existing daily tag {tag}')
    if build > 65535:
        raise ValueError('VRR Windows MSI build field exceeds 65535')


if __name__ == '__main__':
    import os
    tags = subprocess.check_output(['git', 'tag', '--list'], text=True).splitlines()
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--check-vrr':
        check_vrr_version(sys.argv[2], tags)
        sys.exit(0)
    version = next_version(tags)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f'ci_version={version}\ntag=v{version}\n')
    print(f'Allocated {version} (Windows 6.2.{version.rsplit(".", 1)[1]})')

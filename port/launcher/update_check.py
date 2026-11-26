"""Release check: is a newer release of the port out on GitHub?

One request to the GitHub API at launcher start (in the background, 5 s timeout). It sends no data
about the player: a plain GET with a generic User-Agent. Any failure (offline, GitHub down, rate
limit, odd answer, a non-release build) is silent: check() never raises and returns None. Only
builds whose VERSION is a release tag are compared; development and CI builds never report an
update. Settings > Launcher turns the check off.

Release tags: vX.Y.Z-release (since issue #110) and the older vX.Y.Z. Launchers v0.9.1..v0.14.0
broke their home screen when the Latest release was a newer plain vX.Y.Z; they ignore tags with a
suffix, so releases carry '-release' from now on. Keep that suffix while those builds are around.
"""
import json
from pathlib import Path
import re
import subprocess
import urllib.request

REPO = 'tabnul/the-last-guardian-definitive'
API = f'https://api.github.com/repos/{REPO}/releases/latest'
RELEASES = f'https://github.com/{REPO}/releases'
_TAG = re.compile(r'v(\d{1,4})\.(\d{1,4})\.(\d{1,4})(?:-release)?')
_MAX_ANSWER = 256 * 1024


def parse(tag):
    """(major, minor, patch) of a release tag like v0.9.0 or v0.15.0-release; None for anything
    else (dev builds vYYYY.MM.DD-<commit>, empty, not a string)."""
    if not isinstance(tag, str):
        return None
    m = _TAG.fullmatch(tag.strip())
    return tuple(int(part) for part in m.groups()) if m else None


def own_version(port_dir):
    """The first line of the package's VERSION file ('' when there is none)."""
    try:
        return (Path(port_dir) / 'VERSION').read_text(encoding='utf-8').splitlines()[0].strip()
    except (OSError, IndexError, UnicodeDecodeError, TypeError, ValueError):
        return ''


def shown_version(port_dir):
    """The version the launcher shows: the VERSION file of a package; in a source tree
    'dev <commit>' from git (+ ' *' with local changes); '' when neither is there."""
    version = own_version(port_dir)
    if version:
        return version
    try:
        cwd = str(Path(port_dir).parent)
        commit = subprocess.run(['git', 'rev-parse', '--short=7', 'HEAD'], cwd=cwd, capture_output=True,
                                text=True, timeout=5).stdout.strip()
        if not commit:
            return ''
        dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=cwd,
                               capture_output=True, text=True, timeout=5).stdout.strip()
        return f'dev {commit}' + (' *' if dirty else '')
    except Exception:  # noqa: BLE001 - no git, odd folder: no version shown
        return ''


def answer(data):
    """(tag, page url) from a releases/latest answer; None unless the tag is a release tag.
    The page url is only used when it points into this repository's releases."""
    if not isinstance(data, dict) or parse(data.get('tag_name')) is None:
        return None
    url = data.get('html_url')
    if not (isinstance(url, str) and url.startswith(RELEASES + '/') and url.isprintable() and ' ' not in url):
        url = RELEASES
    return data['tag_name'].strip(), url


def latest(timeout=5):
    """(tag, page url) of the newest published release; None on any failure."""
    try:
        request = urllib.request.Request(API, headers={'Accept': 'application/vnd.github+json',
                                                       'User-Agent': 'tlg-launcher'})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            data = json.loads(response.read(_MAX_ANSWER).decode('utf-8'))
        return answer(data)
    except Exception:  # noqa: BLE001 - offline, GitHub down, rate limited, bad JSON: no notice
        return None


def newer(current, release):
    """The release tag when it is newer than the current release build; else None."""
    mine, theirs = parse(current), parse(release)
    return release if mine and theirs and theirs > mine else None


_RELEASE_TAG = re.compile(r'v\d{1,4}\.\d{1,4}\.\d{1,4}-release')


def release_tag_ok(tag):
    """True for the only tag form a release may be published under: vX.Y.Z-release. A plain
    vX.Y.Z as Latest breaks the home page of launchers v0.9.1..v0.14.0 (issue #110); the release
    job runs `python3 update_check.py --release-tag <tag>` and stops on anything else."""
    return isinstance(tag, str) and _RELEASE_TAG.fullmatch(tag) is not None and parse(tag) is not None


def check(port_dir, timeout=5):
    """(tag, url) when a newer release exists for this release build; else None. Never raises."""
    try:
        current = own_version(port_dir)
        if not parse(current):
            return None
        found = latest(timeout)
        if found and newer(current, found[0]):
            return found
    except Exception:  # noqa: BLE001 - the check is optional; it must never stop the launcher
        pass
    return None


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == '--release-tag':
        if release_tag_ok(sys.argv[2]):
            sys.exit(0)
        print(f'::error::release tag {sys.argv[2]!r} is not vX.Y.Z-release: a plain vX.Y.Z as Latest '
              'breaks the home page of launchers v0.9.1..v0.14.0 (issue #110)')
        sys.exit(1)
    print('usage: update_check.py --release-tag <tag>')
    sys.exit(2)

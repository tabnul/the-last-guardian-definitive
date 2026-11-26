# SPDX-License-Identifier: GPL-2.0-or-later
"""Release check (issue #110): a newer release once broke the whole launcher home page. The check
must never raise, whatever GitHub answers (or doesn't), and the notice must never break the page.
The window part needs a display and is skipped without one (CI); run it by hand."""
import io
import json
import os
import socket
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))
import paths  # noqa: E402,F401  (a throwaway launcher settings folder)
import update_check  # noqa: E402

LANG = Path(__file__).resolve().parent.parent / 'launcher' / 'lang'


def answer_of(payload):
    """A fake urlopen() that returns `payload` (bytes) as the response body."""
    class Response(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return False
    return lambda *_, **__: Response(payload)


class Tags(unittest.TestCase):
    def test_release_tags(self):
        self.assertEqual(update_check.parse('v0.14.0'), (0, 14, 0))
        self.assertEqual(update_check.parse('v0.15.0-release'), (0, 15, 0))
        self.assertEqual(update_check.parse(' v1.2.3-release\n'), (1, 2, 3))

    def test_not_release_tags(self):
        for tag in ('', None, 15, ['v0.1.0'], 'v2026.11.26-abc1234', 'v0.15.0-rc1', '0.15.0',
                    'v0.15', 'v0.15.0-release-x', 'v0.15.0\nv9.9.9'):
            self.assertIsNone(update_check.parse(tag), tag)

    def test_old_launcher_ignores_new_style(self):
        """The rule old builds (v0.9.1..v0.14.0) use: a '-release' tag is never 'newer' for them."""
        import re
        old = re.compile(r'^v(\d+)\.(\d+)\.(\d+)$')
        self.assertIsNone(old.match('v0.15.0-release'.strip()))

    def test_newer_across_styles(self):
        self.assertEqual(update_check.newer('v0.14.0', 'v0.15.0-release'), 'v0.15.0-release')
        self.assertEqual(update_check.newer('v0.15.0-release', 'v0.15.1-release'), 'v0.15.1-release')
        self.assertIsNone(update_check.newer('v0.15.0-release', 'v0.15.0-release'))
        self.assertIsNone(update_check.newer('v0.15.0-release', 'v0.14.0'))
        self.assertIsNone(update_check.newer('dev abc1234', 'v9.0.0-release'))


class ReleaseRules(unittest.TestCase):
    """Rules every future release must keep (issue #110)."""
    PORT = Path(__file__).resolve().parent.parent

    def test_release_tag_form(self):
        for tag in ('v0.15.0-release', 'v1.0.0-release', 'v0.14.1-release'):
            self.assertTrue(update_check.release_tag_ok(tag), tag)
        for tag in ('v0.15.0', 'v0.15.0-rc1', ' v0.15.0-release', 'v0.15.0-release\n', 'v0.15-release',
                    'v2026.11.26-abc1234', '0.15.0-release', '', None):
            self.assertFalse(update_check.release_tag_ok(tag), tag)

    def test_release_tag_command(self):
        import subprocess
        script = str(self.PORT / 'launcher' / 'update_check.py')
        run = lambda tag: subprocess.run([sys.executable, '-I', script, '--release-tag', tag],
                                         capture_output=True, text=True).returncode
        self.assertEqual(run('v0.15.0-release'), 0)
        self.assertEqual(run('v0.15.0'), 1)

    def test_release_job_checks_the_tag(self):
        """The release job must run the tag check before it publishes anything."""
        flow = (self.PORT.parent / '.github' / 'workflows' / 'build.yml').read_text(encoding='utf-8')
        release = flow[flow.index('\n  release:'):]
        self.assertIn('update_check.py --release-tag', release)
        self.assertLess(release.index('update_check.py --release-tag'), release.index('gh release create'))

    def test_shipped_version_file_parses(self):
        """package.sh writes the tag as VERSION: the new form must still compare as a release."""
        self.assertEqual(update_check.parse('v0.15.0-release'), (0, 15, 0))

    def test_no_double_format_in_launcher(self):
        """t() formats itself: t('key').format(...) raises KeyError when the text has {fields}
        (the #110 crash). Pass the fields to t() instead."""
        import re
        pattern = re.compile(r"\bt\(\s*'[^']+'\s*\)\s*\.format\(")
        for path in sorted((self.PORT / 'launcher').glob('*.py')):
            for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
                self.assertIsNone(pattern.search(line), f'{path.name}:{number}: {line.strip()}')


class Answers(unittest.TestCase):
    def test_bad_answers_give_none(self):
        for data in (None, [], 'x', {}, {'tag_name': None}, {'tag_name': 7}, {'tag_name': 'nightly'}):
            self.assertIsNone(update_check.answer(data), data)

    def test_url_only_into_our_releases(self):
        ok = update_check.RELEASES + '/tag/v0.15.0-release'
        self.assertEqual(update_check.answer({'tag_name': 'v0.15.0-release', 'html_url': ok})[1], ok)
        for url in (None, 5, 'https://evil.example/x', update_check.RELEASES + '/x y',
                    'https://github.com/someone/else/releases/tag/v1'):
            self.assertEqual(update_check.answer({'tag_name': 'v0.15.0-release', 'html_url': url})[1],
                             update_check.RELEASES, url)

    def test_network_failures_give_none(self):
        errors = (urllib.error.URLError('offline'), socket.timeout('slow'), TimeoutError(), OSError('reset'),
                  urllib.error.HTTPError(update_check.API, 403, 'rate limited', {}, None),
                  ValueError('odd'), RuntimeError('anything'))
        for error in errors:
            with mock.patch('urllib.request.urlopen', side_effect=error):
                self.assertIsNone(update_check.latest(), error)

    def test_odd_bodies_give_none(self):
        for body in (b'', b'not json', b'\xff\xfe', b'[]', b'null', b'{"tag_name": {"x": 1}}',
                     json.dumps({'message': 'Not Found'}).encode()):
            with mock.patch('urllib.request.urlopen', answer_of(body)):
                self.assertIsNone(update_check.latest(), body)


class Check(unittest.TestCase):
    def check_with(self, version, body=None, error=None):
        with tempfile.TemporaryDirectory() as folder:
            if version is not None:
                Path(folder, 'VERSION').write_text(version, encoding='utf-8')
            patch = (mock.patch('urllib.request.urlopen', side_effect=error) if error
                     else mock.patch('urllib.request.urlopen', answer_of(body or b'')))
            with patch:
                return update_check.check(folder)

    def test_finds_newer_release(self):
        body = json.dumps({'tag_name': 'v0.15.0-release',
                           'html_url': update_check.RELEASES + '/tag/v0.15.0-release'}).encode()
        self.assertEqual(self.check_with('v0.14.0\ncommit c6bb7e1\n', body),
                         ('v0.15.0-release', update_check.RELEASES + '/tag/v0.15.0-release'))

    def test_quiet_cases(self):
        body = json.dumps({'tag_name': 'v0.15.0-release'}).encode()
        self.assertIsNone(self.check_with(None, body))                         # no VERSION file
        self.assertIsNone(self.check_with('', body))
        self.assertIsNone(self.check_with('v2026.11.26-abc1234', body))        # CI build
        self.assertIsNone(self.check_with('v0.15.0-release', body))            # up to date
        self.assertIsNone(self.check_with('v0.14.0', error=urllib.error.URLError('offline')))

    def test_never_raises(self):
        with mock.patch.object(update_check, 'latest', side_effect=RuntimeError('boom')):
            with tempfile.TemporaryDirectory() as folder:
                Path(folder, 'VERSION').write_text('v0.14.0', encoding='utf-8')
                self.assertIsNone(update_check.check(folder))
        self.assertIsNone(update_check.check(None))


class Texts(unittest.TestCase):
    def test_every_pack_formats_the_notice(self):
        for path in sorted(LANG.glob('*.json')):
            data = json.loads(path.read_text(encoding='utf-8'))
            text = (data.get('text') or {}).get('launcher.update_available') if isinstance(data, dict) else None
            if text:
                self.assertIn('v0.15.0-release', text.format(tag='v0.15.0-release'), path.name)


class Window(unittest.TestCase):
    def setUp(self):
        try:
            import tkinter as tk
            self.root = tk.Tk()
        except Exception as error:   # no tkinter or no display
            if os.environ.get('TLG_REQUIRE_TK') == '1':   # CI: a skip would hide a broken window
                raise
            self.skipTest(f'no Tk display: {error}')
        import tlg_launcher
        self.tlg = tlg_launcher

    def tearDown(self):
        self.root.destroy()

    def home_works(self, app):
        app.show_page('home')
        self.root.update_idletasks()
        self.root.update()
        self.assertIn('play_box', app.home_items)
        self.assertTrue(app.home.find_withtag('copycmd'))

    def test_notice_shows_and_page_stays_whole(self):
        app = self.tlg.Launcher(self.root)
        app.update_found = ('v0.15.0-release', update_check.RELEASES + '/tag/v0.15.0-release')
        self.home_works(app)
        self.assertTrue(app.home.find_withtag('update'))

    def test_broken_translation_and_bad_notice_data(self):
        app = self.tlg.Launcher(self.root)
        with mock.patch.dict(self.tlg.TEXT, {'update_available': 'Neu {version} {', 'play': '{x}'}):
            for found in (('v0.15.0-release', update_check.RELEASES), ('v1', None), 'junk', (1, 2, 3)):
                app.update_found = found
                self.home_works(app)

    def test_background_check_paths(self):
        """Check raising, None and a hit: the window keeps working; a hit shows the notice."""
        hit = ('v0.15.0-release', update_check.RELEASES)
        for outcome in (RuntimeError('boom'), None, hit):
            app = self.tlg.Launcher(self.root)
            app.update_checked, app.update_found = False, None
            app.settings['update_check'] = True
            kwargs = {'side_effect': outcome} if isinstance(outcome, Exception) else {'return_value': outcome}
            with mock.patch.object(update_check, 'check', **kwargs):
                app.start_update_check()
                for _ in range(40):                     # up to ~2 s for the thread + pick-up
                    self.root.after(50)
                    self.root.update()
                    if app.update_found or outcome is not hit and not any(
                            t.is_alive() for t in __import__('threading').enumerate()[1:]):
                        break
                for _ in range(15):
                    self.root.after(50)
                    self.root.update()
            self.home_works(app)
            self.assertEqual(bool(app.home.find_withtag('update')), outcome is hit, outcome)


if __name__ == '__main__':
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-or-later
"""The new engine's anti-aliasing choice (tlg_run.backend_aa_env): every value tlg.ini can hold reaches the engine as the
game itself loads it (tlg_run.loaded_upscaler, the rule of TlgSettings::Load). With out/gpu/tlg-ini-upscaler built
(ninja -C out/gpu tlg-ini-upscaler), the launcher's rule is compared with the game's own loader for every case."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run  # noqa: E402

# (tlg.ini keys, the upscaler the game loads, TLG_VKB_AA)
CASES = [
    ({}, 'dlss', 'auto'),                                            # fresh launch
    ({'upscaler': 'fsr4'}, 'dlss', 'auto'),                          # a file from before the AA row
    ({'dlaa': '0'}, 'off', 'off'),                                   # from before the row, AA off
    ({'aa_row': '1', 'upscaler': 'dlss'}, 'dlss', 'dlaa'),
    ({'aa_row': '1', 'upscaler': 'fsr3'}, 'fsr3', 'fsr3'),
    ({'aa_row': '1', 'upscaler': 'fsr4'}, 'fsr4', 'fsr4'),
    ({'aa_row': '1', 'upscaler': 'fsr411'}, 'dlss', 'dlaa'),         # FSR 4.1.1: only TLG_UPSCALER sets it; the file loads as DLSS
    ({'aa_row': '1', 'upscaler': 'taa'}, 'taa', 'taa'),
    ({'aa_row': '1', 'upscaler': 'off'}, 'off', 'off'),
    ({'aa_row': '1', 'upscaler': 'xyz'}, 'fsr3', 'fsr3'),            # an unknown name keeps the default
    ({'aa_row': '1'}, 'fsr3', 'fsr3'),
    ({'aa_row': '1', 'dlaa': '0', 'upscaler': 'fsr4'}, 'off', 'off'),
    ({'aa_row': '0', 'upscaler': 'taa'}, 'dlss', 'auto'),
    ({'aa_row': '01', 'upscaler': 'taa'}, 'taa', 'taa'),             # numbers are read as the game's atoi reads them
    ({'aa_row': 'abc', 'upscaler': 'taa'}, 'dlss', 'auto'),
    ({'aa_row': '1', 'dlaa': '00', 'upscaler': 'fsr4'}, 'off', 'off'),
    ({'aa_row': '1', 'dlaa': '2x', 'upscaler': 'fsr4'}, 'fsr4', 'fsr4'),
    ({'aa_row': '1', 'dlaa': '', 'upscaler': 'fsr4'}, 'off', 'off'),
]
GAME_RULE = ROOT / 'out' / 'gpu' / ('tlg-ini-upscaler.exe' if os.name == 'nt' else 'tlg-ini-upscaler')


class BackendAaEnvTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ)
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop('TLG_VKB_AA_CHOICE', None)

    def test_every_value_reaches_the_engine(self):
        for ini, loaded, aa in CASES:
            with self.subTest(ini=ini):
                self.assertEqual(tlg_run.loaded_upscaler(ini), loaded)
                self.assertEqual(tlg_run.backend_aa_env({}, ini).get('TLG_VKB_AA'), aa)

    def test_frame_generation_needs_an_aa(self):
        env = tlg_run.backend_aa_env({}, {'aa_row': '1', 'upscaler': 'off', 'frame_generation': '1'})
        self.assertEqual(env, {'TLG_VKB_AA': 'dlaa', 'TLG_VKB_FG': 'dlss'})
        # TAA with frame generation: the engine has no TAA, it says so and runs FSR 3.1 (frame generation makes its own inputs)
        env = tlg_run.backend_aa_env({}, {'aa_row': '1', 'upscaler': 'taa', 'frame_generation': '1'})
        self.assertEqual(env, {'TLG_VKB_AA': 'taa', 'TLG_VKB_FG': 'dlss'})
        env = tlg_run.backend_aa_env({'frame_rate': '30'}, {'aa_row': '1', 'upscaler': 'off', 'frame_generation': '1'})
        self.assertEqual(env, {'TLG_VKB_AA': 'off'})

    def test_switch_keeps_the_old_table(self):
        os.environ['TLG_VKB_AA_CHOICE'] = '0'
        self.assertEqual(tlg_run.backend_aa_env({}, {'aa_row': '1', 'upscaler': 'fsr411'}), {})
        self.assertEqual(tlg_run.backend_aa_env({}, {'aa_row': '1', 'upscaler': 'taa'}), {})
        self.assertEqual(tlg_run.backend_aa_env({}, {'aa_row': '1', 'upscaler': 'fsr4'}), {'TLG_VKB_AA': 'fsr4'})

    @unittest.skipUnless(GAME_RULE.is_file(), 'out/gpu/tlg-ini-upscaler not built')
    def test_same_rule_as_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            for n, (ini, loaded, _) in enumerate(CASES):
                with self.subTest(ini=ini):
                    path = Path(tmp) / f'{n}.ini'
                    path.write_text(''.join(f'{k}={v}\n' for k, v in ini.items()) or '# empty\n')
                    out = subprocess.run([str(GAME_RULE), str(path)], capture_output=True, text=True, check=True).stdout
                    game = [line.split('=', 1)[1] for line in out.splitlines() if line.startswith('upscaler=')]
                    self.assertEqual(game, [loaded])
                    self.assertEqual(tlg_run.loaded_upscaler(ini), game[0])


if __name__ == '__main__':
    unittest.main()

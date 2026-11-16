# SPDX-FileCopyrightText: Copyright 2026 TLG PC port
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button icons: Xbox (issue #61). The hint picture (gpu/assets/pad_xbox.dds), the glow places in
tlg_lua.cpp and the launcher drawing all come from scripts/make_xbox_pad.py: these tests fail when
one of them is changed without the others."""
import re
import struct
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'launcher'))
import make_xbox_pad  # noqa: E402
import pad_drawing  # noqa: E402

LUA = (ROOT / 'gpu/shim/tlg_lua.cpp').read_text(encoding='utf-8')
PACK = (ROOT / 'gpu/shadps4/video_core/renderer_vulkan/vk_texture_pack.cpp').read_text(encoding='utf-8')


class XboxPadTests(unittest.TestCase):
    def test_glow_table_in_the_game_script_is_the_generated_one(self):
        start = LUA.index('local XBOXPAD={') + len('local XBOXPAD={')
        table = re.sub(r'\s+', '', LUA[start:LUA.index('}}', start) + 1])
        self.assertEqual(table, make_xbox_pad.glow_table())

    def test_every_hint_button_of_the_game_has_a_place(self):
        game = {'R1', 'R2', 'L1', 'L2', 'Left', 'Right', 'Up', 'Down', 'Square', 'Cross', 'Circle', 'Triangle',
                'StickBaseL', 'StickBaseR', 'Push', 'PS', 'Select', 'Start'}
        self.assertEqual(set(make_xbox_pad.GLOW), game)

    def test_picture_is_what_the_loader_expects(self):
        data = (ROOT / 'gpu/assets/pad_xbox.dds').read_bytes()
        height, width = struct.unpack_from('<2I', data, 12)
        levels, = struct.unpack_from('<I', data, 28)
        dxgi, = struct.unpack_from('<I', data, 128)
        self.assertEqual(data[:4], b'DDS ')
        self.assertEqual((width, height), (make_xbox_pad.GAME_W * make_xbox_pad.DETAIL,
                                           make_xbox_pad.GAME_H * make_xbox_pad.DETAIL))
        self.assertEqual(dxgi, 98)  # BC7
        size, w, h = 148, width, height
        for _ in range(levels):
            size += ((w + 3) // 4) * ((h + 3) // 4) * 16
            w, h = max(1, w // 2), max(1, h // 2)
        self.assertEqual(len(data), size)
        # The sizes the loader is told (XboxPictures): the hints picture and the Controls screen (two keys).
        sizes = [(int(w), int(h)) for w, h in re.findall(r'\{0x[0-9a-f]+ull, (\d+), (\d+),', PACK)]
        controls = (ROOT / 'gpu' / 'assets' / 'pad_xbox_controls.dds').read_bytes()
        c_height, c_width = struct.unpack_from('<2I', controls, 12)
        self.assertEqual(sizes, [(width, height), (c_width, c_height), (c_width, c_height)])
        self.assertEqual((c_width, c_height), (make_xbox_pad.C_W, make_xbox_pad.C_ROWS))

    def test_xbox_drawing_has_the_parts_of_the_dualshock(self):
        ids = lambda svg: set(re.findall(r'id="(\w+)"', svg))
        page = {'LeftJoystickInner', 'RightJoystickInner', 'ButtonCross', 'ButtonCircle', 'ButtonSquare',
                'ButtonTriangle', 'PSButton', 'ShareButton', 'OptionsButton', 'LeftShoulder', 'RightShoulder'}
        self.assertLessEqual(page, ids(pad_drawing.XBOX_SVG))
        self.assertLessEqual(page, ids(pad_drawing.DS4_SVG))


if __name__ == '__main__':
    unittest.main()

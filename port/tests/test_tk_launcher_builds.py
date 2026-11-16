# SPDX-License-Identifier: GPL-2.0-or-later
"""The classic (Tk) launcher must build its window. A real display is needed; the test is skipped
without one (CI has none), so run it by hand after touching launcher/tlg_launcher.py or launcher/tlg_run.py (the Qt launcher
work moved the data folder behind tlg_run.user_dir(); the Tk window still reads tlg_run.USER_DIR)."""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))
import paths  # noqa: E402,F401  (a throwaway launcher settings folder)


class TkLauncherBuilds(unittest.TestCase):
    def test_window_builds_and_updates(self):
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as error:   # no tkinter or no display
            self.skipTest(f'no Tk display: {error}')
        try:
            import tlg_launcher
            app = tlg_launcher.Launcher(root)
            root.update_idletasks()
            root.update()
            self.assertTrue(hasattr(app, 'play_button'))
            app.refresh_language_choices()    # also safe when called again after the build
            app.refresh_status()
        finally:
            root.destroy()

    def test_picking_the_backend_sets_its_defaults(self):
        """Renderer picker (merge plan M3 (7), owner 2026-11-16): picking the Vulkan backend sets 4K, the
        uncapped frame rate and the logo skip, and greys their renderer's rows; picking theirs again ungreys."""
        try:
            import tkinter as tk
            root = tk.Tk()
        except Exception as error:
            self.skipTest(f'no Tk display: {error}')
        try:
            import tlg_launcher
            import tlg_run
            app = tlg_launcher.Launcher(root)
            root.update_idletasks()
            box, values = app.combo_boxes['renderer']
            box.current(values.index('vulkan'))
            box.event_generate('<<ComboboxSelected>>')
            root.update()
            for key, value in tlg_run.BACKEND_DEFAULTS.items():
                self.assertEqual(app.settings[key], value, key)
                self.assertEqual(tlg_run.load_settings()[key], value, key)   # saved
            for key in ('resolution', 'frame_rate'):
                shown, shown_values = app.combo_boxes[key]
                self.assertEqual(shown_values[shown.current()], tlg_run.BACKEND_DEFAULTS[key], key)
            self.assertTrue(app.vars['skip_logos'].get())
            self.assertTrue(app.their_widgets)
            self.assertTrue(all(w.instate(['disabled']) for w in app.their_widgets if w.winfo_exists()))
            box.current(values.index('default'))
            box.event_generate('<<ComboboxSelected>>')
            root.update()
            self.assertFalse(app.method_box.instate(['disabled']))
        finally:
            root.destroy()

    def test_auto_continue_reaches_the_game(self):
        import tlg_run
        settings = {**tlg_run.DEFAULT_SETTINGS, 'auto_continue': True}
        saved = os.environ.pop('TLG_AUTO_START', None)
        try:
            self.assertEqual(tlg_run.build_env(settings).get('TLG_AUTO_START'), '1')
            self.assertNotIn('TLG_AUTO_START', tlg_run.build_env({**settings, 'auto_continue': False}))
        finally:
            if saved is not None:
                os.environ['TLG_AUTO_START'] = saved


if __name__ == '__main__':
    unittest.main()

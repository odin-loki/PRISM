"""GUI helpers and optional QWidget smoke test. python -m unittest tests.test_gui -q"""

from __future__ import annotations

import unittest

from helix.gui import skip_from_checks


class TestSkipFromChecks(unittest.TestCase):
    def test_none_checked(self):
        self.assertEqual(skip_from_checks(False, False, False), [])

    def test_all_checked(self):
        self.assertEqual(
            skip_from_checks(True, True, True),
            ["fuzz", "repair", "optional"],
        )

    def test_individual(self):
        self.assertEqual(skip_from_checks(True, False, False), ["fuzz"])
        self.assertEqual(skip_from_checks(False, True, False), ["repair"])
        self.assertEqual(skip_from_checks(False, False, True), ["optional"])

    def test_config_skip_matches_checks(self):
        from helix.config import Config

        skip = skip_from_checks(True, False, True)
        cfg = Config(skip=skip, llm=False)
        self.assertFalse(cfg.want("fuzz"))
        self.assertTrue(cfg.want("bmc"))
        self.assertTrue(cfg.want("repair"))
        self.assertFalse(cfg.want("optional"))
        self.assertFalse(cfg.llm)


try:
    from PySide6.QtWidgets import QApplication

    HAS_PYSIDE6 = True
except ImportError:
    HAS_PYSIDE6 = False


@unittest.skipUnless(HAS_PYSIDE6, "PySide6 not installed")
class TestMainWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._app = QApplication.instance() or QApplication([])

    def test_main_window_constructs(self):
        from helix.gui import MainWindow

        w = MainWindow()
        self.assertFalse(w.skip_fuzz_ck.isChecked())
        self.assertFalse(w.skip_repair_ck.isChecked())
        self.assertFalse(w.skip_optional_ck.isChecked())
        self.assertTrue(w.llm_ck.isChecked())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bhm_gui.py 核心逻辑的无头测试（不开窗口，纯函数断言）。

    python3 tests/test_gui_core.py
"""

import os
import sys
import shutil
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import bhm_gui as B  # noqa: E402

BASH_SAMPLE = "#1728000000\nls -la\n#1728003600\ndocker ps -a\n\n#1728007200\nssh root@10.0.0.1\n"
ZSH_SAMPLE = ": 1728000000:0;ls -la\n: 1728003600:0;docker ps -a\n: 1728007200:0;vim ~/.bashrc\n"
PLAIN_SAMPLE = "ls -la\ncd /tmp\ngit status\n"
MIXED_SAMPLE = "#1728000000\nls\ncd /tmp\nvim x\n"


class TestParse(unittest.TestCase):
    def test_bash(self):
        es = B.parse_bash(BASH_SAMPLE)
        self.assertEqual(len(es), 3)
        self.assertEqual(es[0].ts, 1728000000)
        self.assertEqual(es[0].cmd, "ls -la")
        self.assertEqual(es[2].cmd, "ssh root@10.0.0.1")

    def test_bash_pending_reset(self):
        # 没有时间戳的命令，ts 应为 0（不能继承上一条）
        es = B.parse_bash(MIXED_SAMPLE)
        self.assertEqual([e.ts for e in es], [1728000000, 0, 0])

    def test_zsh(self):
        es = B.parse_zsh(ZSH_SAMPLE)
        self.assertEqual(len(es), 3)
        self.assertEqual(es[1].ts, 1728003600)
        self.assertEqual(es[1].cmd, "docker ps -a")

    def test_zsh_falls_back_plain_line(self):
        es = B.parse_zsh("ls -la\n: 1728000000:0;docker ps\n")
        self.assertEqual(len(es), 2)
        self.assertEqual(es[0].ts, 0)
        self.assertEqual(es[1].ts, 1728000000)

    def test_plain(self):
        es = B.parse_bash(PLAIN_SAMPLE)
        self.assertEqual([e.cmd for e in es], ["ls -la", "cd /tmp", "git status"])
        self.assertEqual([e.ts for e in es], [0, 0, 0])

    def test_crlf(self):
        es = B.parse_bash("#1728000000\r\nls -la\r\n")
        self.assertEqual(es[0].cmd, "ls -la")
        self.assertEqual(es[0].ts, 1728000000)

    def test_empty(self):
        self.assertEqual(B.parse_bash(""), [])
        self.assertEqual(B.parse_zsh(""), [])


class TestDetect(unittest.TestCase):
    def test_detect_bash(self):
        self.assertEqual(B.detect_kind(BASH_SAMPLE, "/root/.bash_history"), B.BASH)

    def test_detect_plain(self):
        self.assertEqual(B.detect_kind(PLAIN_SAMPLE, "/root/.bash_history"), B.PLAIN)

    def test_zsh_by_filename(self):
        # 文件名优先：即使是纯文本也按 zsh 处理
        self.assertEqual(B.detect_kind(PLAIN_SAMPLE, "/root/.zsh_history"), B.ZSH)

    def test_detect_zsh_by_content(self):
        self.assertEqual(B.detect_kind(ZSH_SAMPLE, "/tmp/hist"), B.ZSH)


class TestSerialize(unittest.TestCase):
    def test_bash_roundtrip(self):
        es = B.parse_bash(BASH_SAMPLE)
        out = B.serialize(es, B.BASH)
        self.assertEqual(out, BASH_SAMPLE.replace("\n\n", "\n"))
        self.assertEqual(B.parse_bash(out), es)

    def test_zsh_roundtrip(self):
        es = B.parse_zsh(ZSH_SAMPLE)
        out = B.serialize(es, B.ZSH)
        self.assertEqual(out, ZSH_SAMPLE)
        self.assertEqual(B.parse_zsh(out), es)

    def test_plain_roundtrip(self):
        es = B.parse_bash(PLAIN_SAMPLE)
        out = B.serialize(es, B.PLAIN)
        self.assertEqual(out, PLAIN_SAMPLE)
        self.assertEqual(B.parse_bash(out), es)

    def test_delete_then_serialize(self):
        es = B.parse_bash(BASH_SAMPLE)
        keep = [e for e in es if "docker" not in e.cmd]
        out = B.serialize(keep, B.BASH)
        self.assertEqual(len(B.parse_bash(out)), 2)
        self.assertNotIn("docker", out)


class TestMatch(unittest.TestCase):
    entries = [B.Entry(0, "ls -la"), B.Entry(0, "Docker ps -a"), B.Entry(0, "ssh root@10.0.0.1")]

    def test_plain_case_sensitive(self):
        m = B.Matcher("docker", ci=False)
        self.assertEqual(B.match_indices(self.entries, m), [])

    def test_ci(self):
        m = B.Matcher("docker", ci=True)
        self.assertEqual(B.match_indices(self.entries, m), [1])

    def test_ci_lowercase_keyword_still_ci(self):
        # 关键字本身全小写时，ci 也必须生效（不能退化成大小写敏感）
        m = B.Matcher("ls", ci=True)
        self.assertEqual(B.match_indices(self.entries, m), [0])
        m2 = B.Matcher("LS", ci=True)
        self.assertEqual(B.match_indices(self.entries, m2), [0])

    def test_regex(self):
        m = B.Matcher(r"^Docker", use_regex=True)
        self.assertEqual(B.match_indices(self.entries, m), [1])

    def test_regex_ci(self):
        m = B.Matcher(r"^docker", ci=True, use_regex=True)
        self.assertEqual(B.match_indices(self.entries, m), [1])

    def test_bad_regex(self):
        m = B.Matcher("([", use_regex=True)
        self.assertFalse(m.ok)
        self.assertTrue(m.err)


class TestCsv(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bhmtest.")
        self.out = os.path.join(self.tmp, "e.csv")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_bom_and_header(self):
        es = B.parse_bash(BASH_SAMPLE)
        ok, path = B.export_csv(es, self.out)
        self.assertTrue(ok)
        with open(path, "rb") as f:
            raw = f.read()
        self.assertTrue(raw.startswith(b"\xef\xbb\xbf"), "CSV 必须带 BOM")
        self.assertIn("序号,时间,命令".encode("utf-8"), raw)
        self.assertIn(b"ssh root@10.0.0.1", raw)

    def test_quoting(self):
        es = [B.Entry(0, 'echo "a,b"')]
        ok, path = B.export_csv(es, self.out)
        self.assertTrue(ok)
        with open(path, "r", encoding="utf-8-sig") as f:
            body = f.read()
        self.assertIn('"echo ""a,b"""', body)


class TestBackup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bhmtest.")
        self.old_home = os.environ.get("HOME")
        self.old_up = os.environ.get("USERPROFILE")
        os.environ["HOME"] = self.tmp
        os.environ["USERPROFILE"] = self.tmp  # Windows 分支先看这个

    def tearDown(self):
        if self.old_home is None:
            os.environ.pop("HOME", None)
        else:
            os.environ["HOME"] = self.old_home
        if self.old_up is None:
            os.environ.pop("USERPROFILE", None)
        else:
            os.environ["USERPROFILE"] = self.old_up
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_backup_created(self):
        hist = os.path.join(self.tmp, ".bash_history")
        B.write_file(hist, BASH_SAMPLE)
        bak = B.make_backup(hist)
        self.assertTrue(bak, "备份应成功")
        self.assertTrue(os.path.isfile(bak))
        self.assertTrue(os.path.abspath(bak).startswith(
            os.path.abspath(os.path.join(self.tmp, ".bhm", "backups"))),
            "备份应在 ~/.bhm/backups 下，实际: %s" % bak)
        self.assertEqual(B.read_file(bak), BASH_SAMPLE)

    def test_backup_no_collision(self):
        hist = os.path.join(self.tmp, ".bash_history")
        B.write_file(hist, BASH_SAMPLE)
        a = B.make_backup(hist)
        b = B.make_backup(hist)
        self.assertNotEqual(a, b)
        self.assertTrue(os.path.isfile(a))
        self.assertTrue(os.path.isfile(b))

    def test_backup_missing_file(self):
        self.assertEqual(B.make_backup(os.path.join(self.tmp, "nope")), "")


class TestIo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bhmtest.")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_binary_garbage_survives(self):
        p = os.path.join(self.tmp, "h")
        with open(p, "wb") as f:
            f.write(b"#1728000000\nls -la \xff\xfe\n")
        content = B.read_file(p)
        self.assertIsNotNone(content)
        es = B.parse_bash(content)
        self.assertEqual(len(es), 1)
        self.assertTrue(B.write_file(p, B.serialize(es, B.BASH)))

    def test_read_missing(self):
        self.assertIsNone(B.read_file(os.path.join(self.tmp, "nope")))


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Launcher path/argument contracts, without starting an application or DBI."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'Windows launcher')
class SessionTests(unittest.TestCase):
    def invoke(self, expression):
        return subprocess.run(['powershell.exe', '-NoProfile', '-Command', expression],
                              capture_output=True, text=True)

    def test_spaces_quotes_and_trailing_slashes(self):
        with tempfile.TemporaryDirectory(prefix='ARC arguments ') as temp:
            root = Path(temp)
            (root / 'bin64').mkdir()
            for p in (root / 'bin64/drrun.exe', root / 'client.dll', root / 'target.exe'):
                p.touch()
            literal = lambda s: "'" + str(s).replace("'", "''") + "'"
            values = ['', 'two words', 'literal"quote', 'trailing slash \\', 'a\\"b', '$() & |']
            array = '@(' + ','.join(map(literal, values)) + ')'
            command = (f'& {literal(ROOT / "scripts/arc-cpu-session.ps1")} -ValidateOnly '
                       f'-Executable {literal(root / "target.exe")} -Client {literal(root / "client.dll")} '
                       f'-DynamoRoot {literal(root)} -Output {literal(root / "new evidence")} '
                       f'-ApplicationArguments {array}')
            result = self.invoke(command)
            self.assertEqual(result.returncode, 0, result.stderr)
            output = json.loads(result.stdout)
            argc = ctypes.c_int()
            split = ctypes.windll.shell32.CommandLineToArgvW
            split.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
            split.restype = ctypes.POINTER(ctypes.c_wchar_p)
            ptr = split('runner.exe ' + output['arguments'], ctypes.byref(argc))
            try:
                argv = [ptr[i] for i in range(argc.value)]
                self.assertEqual(argv[-len(values):], values)
                self.assertIn(str(root / 'client.dll'), argv)
            finally:
                free = ctypes.windll.kernel32.LocalFree
                free.argtypes = [ctypes.c_void_p]
                free(ctypes.cast(ptr, ctypes.c_void_p))
            self.assertFalse((root / 'new evidence').exists())

    def test_missing_dependency_fails_before_output(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'untouched'
            result = self.invoke(
                f"& '{ROOT / 'scripts/arc-cpu-session.ps1'}' -ValidateOnly "
                f"-Executable 'C:/Windows/System32/whoami.exe' -DynamoRoot '{temp}' -Output '{output}'")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_stop_mapping(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            control = folder / 'stop.control'
            control.write_bytes(bytes(4))
            script = ROOT / 'scripts/arc-cpu-session.ps1'
            result = self.invoke(
                f"$t=Get-Content -LiteralPath '{script}' -Raw; $tok=$null;$err=$null;"
                "$ast=[System.Management.Automation.Language.Parser]::ParseInput($t,[ref]$tok,[ref]$err);"
                "$a=$ast.Find({param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] "
                "-and $n.Left.Extent.Text -eq '$stopScript'},$true);"
                f"$a.Right.Expression.Value|Set-Content -LiteralPath '{folder / 'stop.ps1'}';"
                f"& '{folder / 'stop.ps1'}'")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(control.read_bytes(), b'\x01\x00\x00\x00')


if __name__ == '__main__':
    unittest.main()

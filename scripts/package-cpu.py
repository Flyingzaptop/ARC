"""Package the CPU vertical slice; does not claim game compatibility or speedup."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--dynamorio', required=True, type=Path)
    parser.add_argument('--client', required=True, type=Path)
    parser.add_argument('--fixture', type=Path)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--gpu-probe', type=Path, help='Separate DX12 observer update; never loaded by the CPU launcher')
    args = parser.parse_args()
    dr = args.dynamorio.resolve()
    for required in (dr / 'bin64/drrun.exe', dr / 'lib64/release/dynamorio.dll',
                     dr / 'License.txt', dr / 'ACKNOWLEDGEMENTS', dr / 'README', args.client,
                     ROOT / 'config/cpu-backend-dependency.json'):
        if not required.is_file():
            parser.error(f'Missing required package input: {required}')
    for subdir in ('bin64', 'lib64', 'ext/lib64', 'ext/bin64'):
        if not (dr / subdir).is_dir():
            parser.error(f'Missing dependency directory: {dr / subdir}')
    dependency = json.loads((ROOT / 'config/cpu-backend-dependency.json').read_text())
    for relative, expected in dependency['runtime_sha256'].items():
        if not (dr / relative).is_file() or sha(dr / relative).lower() != expected.lower():
            parser.error(f'Pinned DynamoRIO runtime mismatch: {relative}')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'bin').mkdir()
    shutil.copy2(args.client, out / 'bin/arc_cpu_client.dll')
    if args.fixture:
        shutil.copy2(args.fixture, out / 'bin/cpu_native_fixture.exe')
    if args.gpu_probe:
        gpu = out / 'gpu-update'
        gpu.mkdir()
        shutil.copy2(args.gpu_probe, gpu / 'arc-dx12-probe.dll')
        (gpu / 'README.txt').write_text('Separate DX12 passive-discovery correction. Not installed automatically. '
            'Do not combine this DLL with the CPU DynamoRIO session. See repository correction evidence.\n')
    runtime = out / 'runtime/dynamorio'
    # Keep official relative layout: drrun locates its core and extensions here.
    for subdir in ('bin64', 'lib64', 'ext/lib64', 'ext/bin64'):
        shutil.copytree(dr / subdir, runtime / subdir,
                        ignore=shutil.ignore_patterns('*.pdb', '*.lib'))
    for name in ('License.txt', 'ACKNOWLEDGEMENTS', 'README'):
        shutil.copy2(dr / name, runtime / name)
    scripts = out / 'scripts'
    scripts.mkdir()
    for name in ('arc-cpu-session.ps1', 'capture-cpu-work.ps1', 'arc-cpu.wprp',
                 'analyze-cpu-regions.py', 'analyze-cpu-session.py'):
        shutil.copy2(ROOT / 'scripts' / name, scripts / name)
    for mode in ('baseline', 'neutral', 'study', 'apply'):
        (out / f'CPU-{mode}.cmd').write_text(
            '@echo off\r\n'
            f'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\\arc-cpu-session.ps1" -Mode {mode} %*\r\n'
            'if errorlevel 1 pause\r\n', encoding='ascii')
    docs = out / 'docs'
    docs.mkdir()
    for name in ('CPU_BACKEND.md', 'CPU_DISCOVERY_CAPTURE.md', 'ARC_IMPLEMENTATION_STATUS.md'):
        shutil.copy2(ROOT / 'docs' / name, docs / name)
    if args.evidence:
        shutil.copytree(args.evidence, out / 'evidence/native')
    (out / 'README-RU.txt').write_text(
        'ARC: экспериментальный CPU backend M2–M4\n\n'
        'CPU-baseline.cmd — исходный запуск без ARC/DBI.\n'
        'CPU-neutral.cmd — цена DynamoRIO без применения преобразований.\n'
        'CPU-study.cmd — ограниченное изучение CPU-участков.\n'
        'CPU-apply.cmd — применение допустимых CPU-преобразований.\n'
        'Auto выполняет ограниченные точные пробы и выбирает действие по замерам\n'
        'внутри DBI; при отсутствии подтверждённой экономии оставляет оригинал.\n'
        'Эти замеры не доказывают выигрыш относительно запуска без ARC.\n'
        'Для диагностической проверки: CPU-apply.cmd -Actuator memo\n'
        '(также specialize и incremental). Это может замедлять приложение.\n'
        'Дважды нажмите нужный файл и выберите EXE. Игра должна быть закрыта.\n'
        'Сессия создаётся в sessions. Для отключения преобразований выполните\n'
        'stop.ps1 из каталога этой сессии; приложение продолжает работать.\n'
        'После обычного закрытия приложения верните весь каталог сессии.\n\n'
        'Это отдельный CPU-запуск: одновременно DX12 DLL ARC не подключать.\n'
        'Защищённые процессы не поддерживаются; обход защиты не выполняется.\n'
        'DBI может существенно замедлять приложение. Успех запуска не означает\n'
        'ускорение игры. Сравнивайте baseline, neutral и apply на одном маршруте.\n'
        'Полный охват памяти, циклов и произвольных функций не заявлен.\n'
        'Сбор ETW запускается отдельно через capture-cpu-work.ps1 с обычным\n'
        'повышением прав. Python 3 нужен только для анализа ETW, не для запуска.\n', encoding='utf-8')
    git = lambda *cmd: subprocess.check_output(['git', *cmd], cwd=ROOT, text=True).strip()
    manifest = {
        'schema': 1, 'source_commit': git('rev-parse', 'HEAD'),
        'source_dirty': bool(git('status', '--porcelain')),
        'source_diff_sha256': hashlib.sha256(subprocess.check_output(
            ['git', 'diff', 'HEAD', '--binary'], cwd=ROOT)).hexdigest(),
        'source_files': {p: sha(ROOT / p) for p in git('ls-files', '--cached', '--others', '--exclude-standard').splitlines()
                         if (ROOT / p).is_file() and (p.startswith(('src/backends/cpu/', 'include/arc/cpu/'))
                         or p in ('include/arc/candidate_contract.hpp', 'include/arc/discovery_budget.hpp',
                                  'scripts/arc-cpu-session.ps1', 'scripts/package-cpu.py', 'config/cpu-backend-dependency.json'))},
        'dependency': dependency,
        'capability_scope': 'CPU register-only bounded regions; see implementation ledger',
        'game_validation': 'pending', 'net_game_benefit': 'unknown',
        'files': {str(p.relative_to(out)).replace('\\', '/'): sha(p)
                  for p in sorted(out.rglob('*')) if p.is_file()},
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(out)


if __name__ == '__main__':
    main()

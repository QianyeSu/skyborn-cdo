#!/usr/bin/env python3
"""
Smart Windows Compatibility Patcher for CDO Source Code

Uses code pattern matching (instead of fixed line numbers) to modify CDO source
for Windows compilation. More resilient to CDO version updates.

Usage:
    python patch_cdo_windows.py apply [--cdo-src PATH]    # Apply patches
    python patch_cdo_windows.py restore [--cdo-src PATH]  # Restore original
    python patch_cdo_windows.py verify [--cdo-src PATH]   # Verify (dry-run)
"""

import re
import sys
import argparse
from pathlib import Path
from typing import Tuple


class WindowsPatcher:
    """CDO Windows compatibility patch manager"""

    def __init__(self, cdo_src_dir: Path):
        self.cdo_src = Path(cdo_src_dir).resolve()
        self.backup_dir = self.cdo_src / ".patch_backup"

    def _backup_map_path(self) -> Path:
        return self.backup_dir / "_path_map.txt"

    def _save_backup_mapping(self, rel_path: str, backup_name: str):
        """Append a rel_path -> backup_name mapping to the map file"""
        with open(self._backup_map_path(), 'a', encoding='utf-8') as f:
            f.write(f"{backup_name}\t{rel_path}\n")

    def _load_backup_mappings(self) -> dict:
        """Load backup_name -> rel_path mappings from the map file"""
        mappings = {}
        map_file = self._backup_map_path()
        if map_file.exists():
            for line in map_file.read_text(encoding='utf-8').splitlines():
                if '\t' in line:
                    backup_name, rel_path = line.split('\t', 1)
                    mappings[backup_name] = rel_path
        return mappings

    def patch_file(self, rel_path: str, patches: list, dry_run: bool = False) -> Tuple[bool, int]:
        """Apply patches to a single file. Returns (success, count)"""
        file_path = self.cdo_src / rel_path

        if not file_path.exists():
            print(f"[X] {rel_path}: File not found")
            return False, 0

        print(f"[*] {rel_path}")

        try:
            content = file_path.read_text(encoding='utf-8', errors='ignore')
        except Exception as e:
            print(f"   [X] Read failed: {e}")
            return False, 0

        original = content
        applied_count = 0

        for desc, pattern, replacement in patches:
            if isinstance(pattern, str):
                # Simple string replacement
                if pattern in content:
                    content = content.replace(pattern, replacement, 1)
                    print(f"   [+] {desc}")
                    applied_count += 1
                else:
                    print(f"   [ ] Not found: {desc}")
            else:
                # Regex replacement
                new_content, count = pattern.subn(
                    replacement, content, count=1)
                if count > 0:
                    content = new_content
                    print(f"   [+] {desc}")
                    applied_count += 1
                else:
                    print(f"   [ ] Not found: {desc}")

        # Write modifications
        if content != original and not dry_run:
            # Backup original file
            backup_name = rel_path.replace('/', '__').replace('\\', '__')
            backup_path = self.backup_dir / backup_name
            backup_path.parent.mkdir(parents=True, exist_ok=True)
            backup_path.write_text(original, encoding='utf-8', newline='\n')
            self._save_backup_mapping(rel_path, backup_name)

            # Write modified content
            file_path.write_text(content, encoding='utf-8', newline='\n')

        return applied_count > 0, applied_count

    def apply_all(self, dry_run: bool = False) -> bool:
        """Apply all patches"""
        print(f"CDO source directory: {self.cdo_src}\n")

        if not dry_run:
            self.backup_dir.mkdir(exist_ok=True)

        total_files = 0
        total_patches = 0

        # =================================================================
        # Patch definitions: based on code pattern matching
        # =================================================================

        patches = [
            # --- src/cdo.cc ---
            ("src/cdo.cc", [
                ("Add Windows headers (io.h, windows.h)",
                 re.compile(r'^(#include\s+<unistd\.h>.*?)$', re.MULTILINE),
                 r'#ifdef _WIN32\n#include <io.h>\n#include <windows.h>\n#else\n\1\n#endif'),

                ("cdo_init_is_tty: Windows implementation",
                 re.compile(
                     r'(static\s+void\s+cdo_init_is_tty\s*\(\s*\)\s*\{)'
                     r'([^}]+)'
                     r'(\})',
                     re.DOTALL
                 ),
                 lambda m: (
                     m.group(1) + '\n#ifdef _WIN32\n'
                     '  cdo::stdinIsTerminal = _isatty(_fileno(stdin));\n'
                     '  cdo::stdoutIsTerminal = _isatty(_fileno(stdout));\n'
                     '  cdo::stderrIsTerminal = _isatty(_fileno(stderr));\n'
                     '#else' + m.group(2) + '#endif\n' + m.group(3)
                 )),

                ("Add fflush before clear_processes",
                 re.compile(
                     r'(\s+)(g_processManager\.clear_processes\s*\(\s*\)\s*;)'),
                 r'\1fflush(stdout);\n\1\2'),
            ]),

            # --- src/cdo_getopt.cc ---
            ("src/cdo_getopt.cc", [
                ("Guard sys/ioctl.h (not available on Windows)",
                 re.compile(r'^(#include\s+<sys/ioctl\.h>)$', re.MULTILINE),
                 r'#ifndef _WIN32\n\1\n#endif'),

                ("Guard unistd.h",
                 re.compile(r'^(#include\s+<unistd\.h>)$', re.MULTILINE),
                 r'#ifndef _WIN32\n\1\n#endif'),
            ]),

            # --- src/process.h: prevent C files from including C++ content ---
            # MinGW's unistd.h includes "process.h" (for Windows process mgmt),
            # which gets resolved to CDO's src/process.h due to -I../../../../src.
            # CDO's process.h is C++ only. Guard it to avoid pulling in <vector>.
            ("src/process.h", [
                ("Guard C++ content from C compiler",
                 re.compile(
                     r'(#ifndef PROCESS_H\s*\n'
                     r'#define PROCESS_H\s*\n)',
                     re.MULTILINE
                 ),
                 r'\1\n#ifdef __cplusplus\n'),

                ("Close C++ guard at end",
                 "#endif /* PROCESS_H */",
                 "#endif /* __cplusplus */\n#endif /* PROCESS_H */"),
            ]),

            # --- libcdi/configure: bypass POSIX.1-2001 check ---
            # MinGW does not define _POSIX_VERSION in <unistd.h>, but libcdi
            # is still buildable.  Force the check result to "yes".
            ("libcdi/configure", [
                ("Bypass POSIX.1-2001 conformance check",
                 "e) acx_cv_cc_posix_support2001=no ;;",
                 "e) acx_cv_cc_posix_support2001=yes ;;"),
            ]),

            # --- libcdi/src/input_file.c: implement pread for Windows ---
            # MinGW doesn't have pread(). The existing "#define pread read" is
            # incorrect (wrong argument count). Replace with proper implementation.
            ("libcdi/src/input_file.c", [
                ("Implement pread for Windows",
                 re.compile(
                     r'// On Windows, define ssize_t and pread manually\s*\n'
                     r'#ifdef _WIN32\s*\n'
                     r'#define ssize_t __int64\s*\n'
                     r'#define pread read\s*\n'
                     r'#include <io\.h>\s*\n'
                     r'#else\s*\n'
                     r'#include <unistd\.h>\s*\n'
                     r'#endif',
                     re.MULTILINE
                 ),
                 '''// On Windows, implement pread using _lseeki64 + _read
#ifdef _WIN32
#include <io.h>
typedef __int64 ssize_t;
typedef __int64 off_t;

static ssize_t pread_windows(int fd, void *buf, size_t count, off_t offset) {
    off_t current = _lseeki64(fd, 0, SEEK_CUR);
    if (current == -1) return -1;
    if (_lseeki64(fd, offset, SEEK_SET) == -1) return -1;
    int result = _read(fd, buf, (unsigned int)count);
    _lseeki64(fd, current, SEEK_SET);
    return result;
}
#define pread pread_windows
#else
#include <unistd.h>
#endif'''),
            ]),

            # --- libcdi/src/gribapi_utilities.c: implement setenv/unsetenv for Windows ---
            # MinGW doesn't have POSIX setenv()/unsetenv(). Implement using _putenv_s().
            ("libcdi/src/gribapi_utilities.c", [
                ("Implement setenv/unsetenv for Windows",
                 re.compile(
                     r'(#include <time\.h>)\s*\n'
                     r'\s*\n'
                     r'(#define FAIL_ON_GRIB_ERROR)',
                     re.MULTILINE
                 ),
                 r'''\1

// Windows compatibility: implement setenv/unsetenv
#ifdef _WIN32
#include <stdlib.h>
static int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
static int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
#endif

\2'''),
            ]),

            # --- Other files: guard unistd.h ---
            *[(f, [("Guard unistd.h",
                   re.compile(r'^(#include\s+<unistd\.h>)$', re.MULTILINE),
                   r'#ifndef _WIN32\n\1\n#endif')])
              for f in [
                  "src/cdo_zaxis.cc",
                  "src/dcw_reader.cc",
                  "src/expr_lex.cc",
                  "src/griddes.cc",
                  "src/merge_axis.cc",
                  "src/operators/CMOR.cc",
            ]],
        ]

        # Apply all patches
        for rel_path, file_patches in patches:
            success, count = self.patch_file(rel_path, file_patches, dry_run)
            if success:
                total_files += 1
                total_patches += count
            print()

        # Summary
        mode = "Verify mode - no files modified" if dry_run else f"{total_files} files modified"
        print(f"{'='*70}")
        print(f"Completed: {mode}, {total_patches} patches applied")
        print(f"{'='*70}")

        return total_files > 0

    def restore_all(self) -> bool:
        """Restore all files from backups"""
        if not self.backup_dir.exists():
            print("No backups found - nothing to restore")
            return True

        print(f"Restoring from backup: {self.backup_dir}\n")

        # Load the mapping file to get correct original paths
        mappings = self._load_backup_mappings()

        restored = 0
        for backup_file in self.backup_dir.iterdir():
            if backup_file.is_file() and backup_file.name != "_path_map.txt":
                # Use mapping to recover original path; fall back to double-underscore split
                rel_path = mappings.get(backup_file.name)
                if not rel_path:
                    rel_path = backup_file.name.replace('__', '/')
                original_file = self.cdo_src / rel_path

                try:
                    content = backup_file.read_text(encoding='utf-8')
                    original_file.write_text(
                        content, encoding='utf-8', newline='\n')
                    print(f"[+] Restored: {rel_path}")
                    restored += 1
                except Exception as e:
                    print(f"[X] Restore failed {rel_path}: {e}")

        # Clean up backup directory
        try:
            for f in self.backup_dir.iterdir():
                f.unlink()
            self.backup_dir.rmdir()
            print(
                f"\n[OK] {restored} files restored, backup directory removed")
        except Exception as e:
            print(
                f"\n[!] {restored} files restored, but failed to remove backup dir: {e}")

        return True


def main():
    parser = argparse.ArgumentParser(description="CDO Windows Smart Patcher")
    parser.add_argument("action", choices=["apply", "restore", "verify"],
                        help="Action: apply/restore/verify")
    parser.add_argument("--cdo-src", type=Path,
                        help="CDO source directory (default: ../vendor/cdo)")

    args = parser.parse_args()

    # Determine CDO source directory
    if args.cdo_src:
        cdo_src = args.cdo_src
    else:
        script_dir = Path(__file__).parent
        cdo_src = script_dir.parent / "vendor" / "cdo"

    if not cdo_src.exists():
        print(f"[X] CDO source directory not found: {cdo_src}")
        print("   Please specify --cdo-src or ensure vendor/cdo exists")
        return 1

    patcher = WindowsPatcher(cdo_src)

    if args.action == "apply":
        success = patcher.apply_all(dry_run=False)
    elif args.action == "restore":
        success = patcher.restore_all()
    elif args.action == "verify":
        success = patcher.apply_all(dry_run=True)

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())

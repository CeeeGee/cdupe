# cdupe

A small, dependency-free duplicate file finder written in C. It reports duplicate files and can optionally replace them with hardlinks to reclaim disk space.

## How it works

Comparing every file with every other file would be slow, so cdupe narrows things down in stages, doing the expensive work only on files that survive the cheaper checks:

1. **Group by size.** Files with a unique size cannot have a duplicate, so they are dropped.
2. **Hash the first 4 KB** (FNV-1a). Files whose partial hashes differ are dropped.
3. **Hash the whole file.** Files with matching full hashes are duplicate candidates.
4. **Byte-by-byte comparison** (only with `--link`), so a hash collision can never cause data loss.

## Build

Requires a C compiler (gcc or clang) and make. No other dependencies.

```
make
```

## Usage

```
./cdupe [options] [folder]
```

| Option | Meaning |
|---|---|
| `--link` | Replace duplicates with hardlinks. Without this flag, nothing is modified. |
| `--min-size N` | Ignore files smaller than N bytes (default: 1, so empty files are skipped) |
| `--hidden` | Also scan hidden files and folders (skipped by default) |
| `-h`, `--help` | Show help |

## Example

```
$ ./cdupe ~/testdata
Scanned 5 files

Duplicate group (6 bytes each, 3 files):
  keep  /home/user/testdata/a.txt
  dup   /home/user/testdata/d.txt
  dup   /home/user/testdata/sub/b.txt

1 duplicate group(s), 12 bytes reclaimable
Dry run only. Re-run with --link to replace duplicates with hardlinks.
```

## Safety

- Dry run is the default. Files are only changed with `--link`.
- Symlinks are never followed.
- Files that are already hardlinked to each other are skipped, so savings are not overcounted.
- Duplicates on a different filesystem are skipped, since hardlinks cannot cross filesystems.
- Replacement is crash-safe: the new link is created under a temporary name and then renamed over the duplicate, so a file is never missing.

## Limitations

- After `--link`, the linked files are the same file. Editing one in place changes all of them. Use `--link` on files you do not modify (archives, downloads, photo libraries), not on working files.
- The linked duplicates take on the permissions, owner and timestamps of the kept file.
- The kept file is whichever sorts first. You cannot choose it.
- Paths longer than 4096 characters are not supported.
- Do not run `--link` on cloud-synced or backup-managed folders.

## Platforms

Tested on: Ubuntu 24.04 (WSL2).
Should work on other POSIX systems (Linux, macOS, BSD), but this is untested.
Native Windows is not supported (use WSL2).

# Windows rules

Ported by hand from upstream `docs/windows-rules.md` (autonomous-ai/openharness, commit 1347c24810), rule 1 only. The other
rules there belong to code this branch does not have (the Windows installer runner, `hidden_process.dart`, the signed launcher
set).

## 1. Harness never opens a window the person did not ask for

On a Mac a background program has no window. On Windows a console program started by a process that has no
console gets a new console, and that window flashes up and closes. The person sees it every time, and they
cannot tell what it was.

Measured upstream on Windows 11 with Windows Terminal as the default terminal (2026-11-05). A Node parent with a hidden
console, as the daemon has, starts `cmd`, and `cmd` starts `ping` and `powershell`:

| Node `spawn` options | A window flashes? |
|---|---|
| `detached: true` | **Yes** |
| `detached: true, windowsHide: true` | **Yes** |
| none | No |
| `windowsHide: true` | No |

Why: libuv turns `detached` into `DETACHED_PROCESS`, so the child has no console at all, and each console
program it starts then gets a new one. `CREATE_NO_WINDOW` is ignored beside `DETACHED_PROCESS`, so
`windowsHide` cannot rescue it. It looks hidden in a quick test when the child itself starts nothing.

The rule for the CLI (Node):

- **Never `detached: true` on Windows for a program that starts other programs.** Use `ownProcessGroup()` from
  `cli/src/lib/windows/noWindow.ts`: its own process group on macOS and Linux, a hidden console on Windows.
  `cli/src/lib/windows/noWindow.spec.ts` fails on a new `detached: true` anywhere in `cli/src` unless that file lists it
  with the reason.
- **A process that must outlive Node** (Node ends a child that is not detached when Node exits) starts through
  `bin\harness-launch.exe` (`cli/src/lib/windows/launcher.ts`), which uses `CREATE_NO_WINDOW`, and only a program that
  starts no console program of its own may stay `detached: true, windowsHide: true`.

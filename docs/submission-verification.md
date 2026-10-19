# Submission verification — 2026-10-19

Host-only round 2. No flash, monitor, erase, device contact, provider request or registry query. Builds and host suites use a network-isolated Bubblewrap namespace with private `/dev` and read-only `sdkconfig.local`. Existing untracked files are not submission changes.

## Diagnostics

- The transcript remains a `role="log"` live region. A “Read recent conversation” anchor focuses its `tabindex="-1"` target, following the app's existing skip-link pattern. Svelte check reports **0 errors / 0 warnings** (previously one accessibility warning). Actual Chromium on the static site with isolated REST responses: keyboard Enter moved focus to the log, PageDown moved `scrollTop` from 0 to 227, and the visible focus outline measured 3 px. The smoke inserted local overflow text; it did not read a device transcript or exercise a screen reader.
- **Retained:** `svelte-focus-trap@1.2.0` declares `svelte: src/index.js` but no `exports` map. The installed Vite Svelte plugin warns by inspecting package metadata. The local npm cache contains only the 1.2.0 tarball; a newer release was not verified because network access is prohibited. No supported local metadata override was found. Patching `node_modules` would be non-reproducible and is prohibited; disabling the warning would only hide it. No package version, lockfile or dependency files were changed.
- Both Vite middleware fixtures previously set `hmr: false`, but Vite 5.4.19 still creates a WebSocket server and falls back to port 24678. Each now supplies its own Node HTTP server listening on port 0 at 127.0.0.1 and closes it after Vite. Five concurrent-file repetitions passed **27/27 each**, with no `WebSocket server error: Port is already in use`. Assertions and test counts were unchanged.

## Interface gates

All exited 0 after the fixture changes:

| Command | Result |
| --- | --- |
| `npm --offline --no-audit --no-fund --prefix interface ci` | 255 packages installed from local cache; no registry access |
| `npm --prefix interface run check` | 0 errors / 0 warnings; separate dependency metadata warning retained |
| From `interface`: `node --test test/*.test.mjs` | 29 passed, 0 failed/cancelled/skipped/todo |
| `npm --prefix interface run build` | Static production site generated |

The offline flags constrain dependency installation for this verification; ordinary `npm --prefix interface ci` remains the fresh-machine instruction. These results do not establish physical-board behavior or live inference.

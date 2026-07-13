# token-mode branch (scoot-scoot fork)

This branch adds **Token Mode** for the [scoot-scoot](https://github.com/smoll/scoot-scoot)
MTG e-ink clone-card project: a home-menu activity that joins Wi-Fi, serves
the scoot-scoot PWA from flash, and accepts full-screen 1-bit BMP pushes on
`POST /display`. Protocol: `docs/PROTOCOL.md` in the scoot-scoot repo.

## ⚠️ Never open a PR against upstream

This branch lives **only** in the `smoll/crosspoint-reader` fork. Token mode
is deliberately out of scope for the upstream e-reader project (see their
`SCOPE.md`) — **do not submit it, or any part of it, upstream**. Maintenance
model: periodically rebase `token-mode` onto upstream `master` to pick up
fixes; resolve conflicts locally; force-push to the fork.

## What's on this branch

New files (additive):

- `src/activities/token/TokenModeActivity.{h,cpp}` — the activity (Wi-Fi join,
  idle screen with QR, HTTP pump, 10-min idle exit)
- `src/network/TokenModeServer.{h,cpp}` — HTTP server: `POST /display`,
  `GET /api/token-status`, embedded PWA with SPA fallback
- `src/network/TokenBmpDecoder.{h,cpp}` — streaming 1-bit BMP decoder
  (host-testable, no Arduino deps)
- `src/network/TokenWebAssets.embedded.h` — **generated** gzipped PWA
  (regenerate with `node tools/embed-webassets.mjs` in scoot-scoot after any
  web change; committed here so the firmware builds without a JS toolchain)
- `test/token_bmp_decoder/` — gtest suite for the decoder

Upstream files touched (kept minimal for rebases):

- `src/activities/ActivityManager.{h,cpp}` — `HomeMenuItem::TOKEN_MODE`, `goToTokenMode()`
- `src/activities/home/HomeActivity.{h,cpp}` — home-menu entry
- `lib/I18n/translations/english.yaml` — `STR_TOKEN_MODE`, `STR_TOKEN_MODE_HINT`
- `test/CMakeLists.txt` — registers the decoder suite

# Contributing

Thanks for helping. This project runs on real consoles, so the bar for safety
is higher than for features.

## Before you start

* Read [docs/safety-model.md](docs/safety-model.md). Changes that weaken it
  will not be merged.
* Larger changes: open an issue first to agree on the approach.
* Respect the phase plan ([docs/architecture.md](docs/architecture.md)).
  Overlay activation (Phase 5) is not started until Phases 1–4 have been
  tested on hardware.

## Development setup

See [docs/building.md](docs/building.md). In short:

```sh
cmake -S . -B build -G Ninja -DAKENO_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/mock_shadowmount.py &   # fake ShadowMountPlus API for the desktop build
./build/src/ui/AkenoModManager --data-root /tmp/akeno
```

## Rules for code

* C++20. Follow the style of the surrounding code (4-space indent,
  `PascalCase` types, `camelCase` functions, trailing `_` for members).
* Return `Result<T>` / `Status`. No exceptions across module boundaries.
  Errors carry a user-facing `message` and a technical `detail`.
* All file-system writes go through `security::SafeFs`. Never call `open`,
  `rename`, `remove` etc. for writing directly.
* All network access goes through `network::IHttpClient`. TLS verification
  may not be disabled.
* Untrusted input (network, archives, catalogue) is size-limited before
  allocation and validated field by field.
* No `#ifdef AKENO_TARGET_PS5` outside `src/platform/` and `src/ui/sdl/`.
* Never log secrets. The redactor is a safety net, not permission.
* New behaviour comes with tests. The suite must pass on the host, including
  under `-DAKENO_SANITIZE=ON`.

## Reporting test results honestly

Use the status words from [docs/compatibility.md](docs/compatibility.md):
`compiled`, `unit tested`, `mock tested`, `hardware tested`, `verified`.
Do not describe something as working on a firmware version unless you ran it
there. Hardware test reports go into the log in that document.

## Commit messages

Conventional style: `feat: …`, `fix: …`, `docs: …`, `test: …`, `build: …`,
`chore: …`. One logical change per commit.

## License

By contributing you agree that your contribution is licensed under
GPL-3.0-or-later.

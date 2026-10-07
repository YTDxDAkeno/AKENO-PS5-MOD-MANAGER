# Security Policy

Akeno PS5 Mod Manager runs with elevated privileges on a jailbroken console and
processes data from the internet. Security reports are taken seriously.

## Reporting a vulnerability

Please **do not open a public issue** for a vulnerability. Use GitHub's
private vulnerability reporting ("Security" tab → "Report a vulnerability") on
this repository. Include:

* the affected version (`About` screen or `--version`),
* what an attacker controls (a catalogue entry, a mod archive, a server
  response, a file on the console),
* what happens (write outside the application directory, code execution,
  crash, data loss),
* steps or a proof of concept, if you have one.

You can expect an acknowledgement within a week. Fixes for issues that could
damage console data or execute code are prioritised above all feature work.

## In scope

* Any write outside `/data/akeno-mod-manager/` (or the configured data root).
* Path traversal or symlink escapes (catalogue data, archives, the recovery
  journal).
* Execution of downloaded content.
* TLS validation bypasses, plain-HTTP use outside loopback, redirect abuse.
* Memory exhaustion from oversized responses or images.
* Credentials or tokens appearing in logs or diagnostic exports.
* Exposure of the ShadowMountPlus API beyond the console.

## Out of scope

* Vulnerabilities in the PS5 firmware, the jailbreak, kstuff or
  ShadowMountPlus itself. Please report those to their maintainers.
* Mods behaving badly inside a game. Akeno guarantees non-destructive
  installation, not the quality of third-party mods.

## Design references

* [docs/safety-model.md](docs/safety-model.md): rules every change must follow
* [docs/shadowmount.md](docs/shadowmount.md): the overlay contract

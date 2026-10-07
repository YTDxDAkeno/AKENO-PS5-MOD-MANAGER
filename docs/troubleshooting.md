# Troubleshooting

Start with the logs: `/data/akeno-mod-manager/logs/akeno.log`, or
**Settings → View the log** on the console. **Settings → Export diagnostic
log** writes a single file with versions, settings, the last system check and
recent log lines (secrets removed) for bug reports.

## "ShadowMountPlus is not answering on port 10101"

* Is ShadowMountPlus running? It must be loaded after every reboot (payload
  loader or plk-autoloader).
* Is it version **1.7 or newer**? Older versions have no API. Akeno reports
  *"This ShadowMountPlus version has no API"* when it gets HTTP 404.
* Did you change `api_port` in `/data/shadowmount/config.ini`? Set the same
  port in **Settings → ShadowMountPlus API port** and restart Akeno.
* `api_enabled=0` in the ShadowMountPlus config disables the API completely.
* Akeno always connects to `127.0.0.1`. There is no need to bind the API to
  the network.

Press **OPTIONS** in the Games tab to check again.

## "The game library is not available" but ShadowMountPlus runs

The System Check line *ShadowMount API* gives the reason:

* *API version N is not supported*: a future ShadowMountPlus changed its API.
  Akeno refuses to guess. Please report it.
* *capability 'list_games' missing*: this ShadowMountPlus build does not offer
  the game list.

## A game shows no icon

Akeno asks ShadowMountPlus for the icon. Icons larger than 4 MB, wider or
taller than 4096 px, or not PNG/JPEG are refused and a placeholder is shown.
Icons are cached in `cache/icons/`. Deleting that folder is safe.

## "Updated" badge on a game

The game's version differs from the one Akeno saw before. The game details
show the previous version. In later releases this triggers a compatibility
re-check of installed mods.

## Networking shows FAILED

The check makes one HTTPS request to `https://raw.githubusercontent.com/`
with certificate verification. Failure messages:

* *Could not find …*: DNS or no internet connection.
* *The secure connection … could not be verified*: TLS verification failed.
  Check the console clock. Akeno never disables verification.

Networking is not needed for the game library.

## Discover says "Could not load the Akeno Catalogue"

* *There is no Akeno Catalogue at this address*: the catalogue address in
  Settings is wrong, or the server has no `index.json` there. Leave the
  address empty in Settings to return to the default.
* Connection and certificate errors: see "Networking shows FAILED" above.
* The catalogue is cached for ten minutes; OPTIONS in Discover reloads it.

## "The catalogue is empty"

The published catalogue has no entries until mods have been tested on PS5
hardware. This is expected in 0.1.0-alpha.

## The search keyboard does not open (PS5)

Akeno opens the console's keyboard dialog. If it does not appear within a few
seconds, a message offers to try again (✕) or cancel (○). Please report it with
the log, since this is not hardware tested yet.

## "The database was created by a newer version"

You ran a newer Akeno before. The database is left untouched. Update Akeno, or
move `database/akeno.sqlite` away to start fresh (you lose settings only).

## "Interrupted … detected" at startup

The previous session stopped during an operation. The screen tells you whether
the active overlay could have been affected. **Clean up staging data** removes
only the temporary folders that operation created inside `staging/`.

## The controller does not respond

The PS5 SDL port exposes the pad as a raw joystick. Akeno maps its button
order (✕ ○ □ △, OPTIONS, L1/R1, D-pad) and the left stick. This mapping
comes from the port's source and is **not yet hardware tested**. Please
report which buttons do what.

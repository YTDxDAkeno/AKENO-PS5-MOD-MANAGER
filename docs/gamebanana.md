# GameBanana in Akeno (0.2.0-alpha)

[GameBanana](https://gamebanana.com) is a free mod site: no account, no key, free
downloads. Akeno uses its public API (`/apiv11`).

**Off by default.** To find your games there, Akeno has to send the **names of
your installed games** to gamebanana.com. Switch it on in *Settings → GameBanana
mods (free)* only if that is fine with you; switching it off stops all requests.

**Not hardware tested, and not tested against the live site** (the development
environment cannot reach gamebanana.com); built against the API as GameBanana's
own site and PC mod managers use it, tested with recorded answers.

* Discover shows your games that exist on GameBanana, marked "(GameBanana)".
* Mods: newest first by default, with search (GameBanana's own name filter);
  NSFW mods are left out; descriptions are shown as plain text.
* Files: only `.zip` and `.7z` (`.rar` is not supported). Downloads go through
  `https://gamebanana.com/dl/<file>` over HTTPS.
* GameBanana publishes no SHA-256: Akeno checks the exact size, records the
  SHA-256, then unpacks and analyses as for every mod.
* Like Nexus, GameBanana mods are made for PC: **EXPERIMENTAL**, with a
  confirmation for downloading; Windows programs, script loaders and console code
  are refused. The check maps the archive onto the installed game and explains
  the result; PC files are activated only with recorded evidence for the game
  version (see [compatibility-engine.md](compatibility-engine.md)), or installed
  as a user-confirmed test install when they qualify for one.

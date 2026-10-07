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
* Like Nexus, GameBanana mods are made for PC: always **EXPERIMENTAL**, with a
  confirmation for downloading and for installing; Windows programs, script
  loaders and console code are still refused. Archives are installed as they are
  laid out; the check screen shows where every file goes.

/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Akeno PS5 Mod Manager entry for the ps5-payload-dev/websrv Homebrew Launcher.
 * Install the whole AkenoModManager folder to /data/homebrew/AkenoModManager/.
 * The folder intentionally has no sce_sys/param.json: ShadowMountPlus also scans
 * /data/homebrew and would otherwise try to register it as a game.
 */

async function main() {
    const WORKDIR = window.workingDir;

    return {
        mainText: "Akeno PS5 Mod Manager",
        secondaryText: "0.1.0-alpha (experimental) - game library browser",
        onclick: async () => {
            return {
                path: WORKDIR + "/eboot.elf",
                cwd: WORKDIR,
                args: [],
                env: {HOME: WORKDIR}
            };
        }
    };
}

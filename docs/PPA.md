# Publishing SUCO to a Launchpad PPA

This makes SUCO installable the canonical Ubuntu way:

```bash
sudo add-apt-repository ppa:YOURNAME/suco
sudo apt update && sudo apt install suco
```

Unlike the self-hosted GitHub Pages apt repo (which already works — see the README),
a Launchpad PPA is Ubuntu-only, but Launchpad builds the binaries for you across
multiple Ubuntu series and architectures, and users get the familiar
`add-apt-repository ppa:` one-liner.

The `debian/` packaging in this repo is ready; the steps below are **your** part
(they need your Launchpad account and GPG key — nothing here can do them for you).

## One-time setup

1. **Launchpad account** — sign up at <https://launchpad.net>. Pick your username;
   the PPA will be `ppa:<username>/suco`.

2. **GPG key** — Launchpad signs uploads with your key.
   ```bash
   gpg --full-generate-key            # ed25519 or RSA 4096
   gpg --list-secret-keys --keyid-format long   # note the key id
   gpg --send-keys --keyserver keyserver.ubuntu.com <KEYID>
   ```
   Then on Launchpad: *Profile → Change details → OpenPGP keys*, paste the key
   fingerprint, and confirm the encrypted email Launchpad sends.

3. **Create the PPA** — Launchpad → your profile → *Create a new PPA* → name it
   `suco`.

4. **Upload tool**
   ```bash
   sudo apt install devscripts debhelper dput
   ```

## Build and upload (per Ubuntu series)

On a Debian/Ubuntu machine (a grid node works) with a clean checkout:

```bash
sudo apt install cmake g++ libssl-dev libzstd-dev libsqlite3-dev
scripts/make_ppa_source.sh noble 1        # builds a signed SOURCE package
dput ppa:YOURNAME/suco ../suco_*~noble1_source.changes
```

Launchpad emails you the build result (usually a few minutes). Repeat per series
you want to support (`jammy`, `noble`, …) — each needs its own upload with the
series name; `make_ppa_source.sh` bumps the version suffix (`~noble1`, `~jammy1`).
Bump the trailing number (`noble2`) to re-upload a fixed package to the same series.

## Notes

- The packaging is **native** (`3.0 (native)`) — the whole project is the source,
  no separate upstream tarball. `debian/source/options` keeps `build*/`,
  `thirdparty/`, and `.git` out of the uploaded tarball.
- Services install **disabled** (`debian/rules` uses `dh_installsystemd
  --no-enable --no-start`) — same as the .deb: a fresh install never auto-joins a
  grid. Users enable a role with `systemctl enable --now suco-worker`.
- Build-deps mirror the CMake `find_package` requirements: OpenSSL, zstd, SQLite3.
- Test the source build locally before uploading:
  `scripts/make_ppa_source.sh noble 1` should produce `../suco_*_source.changes`
  with no errors. To dry-run the *binary* build too: `debuild -b -us -uc`.
- The GitHub Pages apt repo and the PPA can coexist — Debian users take the repo,
  Ubuntu users take `ppa:`.

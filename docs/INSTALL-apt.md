# Install SUCO with `apt`

```bash
curl -fsSL https://micbur.github.io/suco/suco-archive-keyring.asc \
  | sudo tee /etc/apt/keyrings/suco.asc >/dev/null
echo "deb [signed-by=/etc/apt/keyrings/suco.asc] https://micbur.github.io/suco stable main" \
  | sudo tee /etc/apt/sources.list.d/suco.list >/dev/null
sudo apt update
sudo apt install suco
```

Replace `micbur.github.io/suco` with your Pages URL once the repo is published.

Installing does **not** start anything. Pick each machine's role:

```bash
# a compile node:
sudo systemctl enable --now suco-worker
# the head node (coordinator + a worker):
sudo systemctl enable --now suco-coordinator suco-worker
```

Dashboard: `http://<coordinator-host>:9001`

---

## Maintainer: one-time publishing setup

The `Publish APT repo` workflow (`.github/workflows/release-apt.yml`) builds the
`.deb`, signs the repository, and deploys it to GitHub Pages on every `v*` tag.
Before the first release:

1. **Create a signing key** (once, keep the secret key safe — it identifies your repo):
   ```bash
   gpg --full-generate-key          # choose: ECC (sign only) → Curve 25519
   gpg --list-keys                  # note the key id
   ```
2. **Add the private key as a repo secret** so the workflow can sign:
   ```bash
   gpg --armor --export-secret-keys <keyid>
   ```
   Copy the output into **Settings → Secrets and variables → Actions →
   New repository secret**, name `APT_GPG_PRIVATE_KEY`.
3. **Enable Pages**: **Settings → Pages → Source → GitHub Actions**.
4. **Release**:
   ```bash
   git tag v0.9.1 && git push origin v0.9.1
   ```
   The workflow publishes to `https://<user>.github.io/<repo>`. The public key is
   served at `…/suco-archive-keyring.asc` automatically — that is what the install
   commands above fetch.

Build a repo locally without CI: `scripts/build_apt_repo.sh <deb> <out-dir> <keyid>`.

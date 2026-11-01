# Deployment

Two GitHub Actions workflows:

| Workflow | Runs on | Does |
|---|---|---|
| `.github/workflows/ci.yml` | every push to other branches, every PR | build + all tests (Ubuntu 22.04/24.04, Release/Debug), accuracy and speed smoke tests, Go gateway build, `shellcheck` of the deploy script, compose-file validation |
| `.github/workflows/deploy.yml` | push to `master`/`main`, tags `v*.*.*`, manual run | CI as a gate → Docker image → push to GHCR → smoke-test the image → SSH deploy to the server with health check and automatic rollback |

```
push to master ──► CI (all tests) ──► build image ──► ghcr.io/rabbicse/fp-extractor:sha-abc1234
                                                     (+ :master, :latest, :1.2.3 for tags)
                                     smoke test: run image, GET /healthz, /v1/stats
                                          │
                                          ▼  environment "production" (optional manual approval)
                          ssh server: remote-deploy.sh ghcr.io/...:sha-abc1234
                              docker compose pull && up -d
                              wait for /healthz (up to 300 s: 1 M templates reload into RAM)
                              healthy?  yes → record as current      no → roll back to previous image
```

Deployments always use the immutable `sha-xxxxxxx` tag, so what is running is
exactly the commit that passed the tests, and a rollback is exact.

## One-time setup

### 1. Server (any Linux VM with Docker)

```bash
# Docker Engine + compose plugin + curl
curl -fsSL https://get.docker.com | sh
sudo apt-get install -y curl

# a deploy user that may run docker
sudo useradd -m -s /bin/bash deploy && sudo usermod -aG docker deploy
sudo -u deploy mkdir -p ~deploy/fp-extractor ~deploy/.ssh

# the API key lives only on the server
sudo -u deploy sh -c 'umask 077; echo "FPX_API_KEY=$(openssl rand -hex 32)" > ~/fp-extractor/.env'
```

Optional settings in `~/fp-extractor/.env`: `FPX_PORT` (default 8080),
`FPX_BIND` (default `127.0.0.1`), `FPX_MATCH_THREADS` (default 0 = all cores).

The container listens on `127.0.0.1:8080` only. Put a TLS reverse proxy in
front, for example Caddy (automatic HTTPS):

```
# /etc/caddy/Caddyfile
fpx.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

Sizing: about 1.5 GB RAM per million finger templates (×2 headroom) and as
many cores as your latency target needs (see [SCALING.md](SCALING.md)).

### 2. SSH key for GitHub Actions

```bash
ssh-keygen -t ed25519 -N '' -f fpx_deploy -C github-actions-deploy
# on the server:
cat fpx_deploy.pub | sudo -u deploy tee -a ~deploy/.ssh/authorized_keys
# pin the server's host key:
ssh-keyscan -p 22 your.server.example.com > known_hosts
```

### 3. GitHub settings

*Settings → Environments → New environment* → `production`
(add **required reviewers** if every deployment should wait for approval).

*Settings → Secrets and variables → Actions*:

| Kind | Name | Value |
|---|---|---|
| Repository **variable** | `DEPLOY_HOST` | `your.server.example.com` (**required**; the deploy job is skipped while it is empty) |
| Repository variable | `DEPLOY_USER` | `deploy` |
| Repository variable | `DEPLOY_PORT` | `22` (optional) |
| Repository variable | `DEPLOY_PATH` | `~/fp-extractor` (optional) |
| Repository variable | `DEPLOY_HEALTH_URL` | `http://127.0.0.1:8080/healthz` (optional) |
| Environment secret (`production`) | `DEPLOY_SSH_KEY` | contents of `fpx_deploy` (private key) |
| Environment secret (`production`) | `DEPLOY_KNOWN_HOSTS` | contents of `known_hosts` |
| Environment secret (`production`) | `GHCR_PULL_TOKEN` | only while the GHCR package is private: a PAT with `read:packages` |

`DEPLOY_HOST` must be a repository variable (not an environment variable)
because the job-level `if:` that decides whether to deploy is evaluated before
the environment is attached.

The image is published to `ghcr.io/rabbicse/fp-extractor`. New packages are
private: either make it public (*Packages → fp-extractor → Package settings*)
or set `GHCR_PULL_TOKEN`.

## Everyday use

* **Deploy:** merge to `master`. Watch *Actions → Deploy*.
* **Release:** `git tag v2.1.0 && git push origin v2.1.0` → image also tagged `2.1.0`.
* **Deploy a branch or older tag:** *Actions → Deploy → Run workflow*, pick the ref.
* **Manual rollback:** re-run the Deploy workflow of an older commit, or on the server:
  `~/fp-extractor/remote-deploy.sh ghcr.io/rabbicse/fp-extractor:sha-<old>`.
* **What is running:** `cat ~/fp-extractor/.deploy-state`.

## Data safety

* Templates are in the Docker volume `fpx-data` (`/data/gallery.fpxdb`); it
  survives deploys and rollbacks. **Back it up**, e.g. nightly:
  `docker run --rm -v fp-extractor_fpx-data:/d -v /backup:/b ubuntu tar czf /b/fpx-$(date +%F).tgz -C /d .`
* Compact occasionally (drops superseded/deleted records), through the
  running server so the open file is swapped safely:
  `curl -X POST -H "X-API-Key: $KEY" http://127.0.0.1:8080/v1/admin/compact`.
  Writes wait while it runs (seconds per million records); searches continue.
  **Never run `fpx compact` on the file of a running server**: the server
  would keep appending to the replaced file and those writes would be lost.
* Deploys restart the single instance: identification is unavailable while
  templates reload (seconds to ~30 s per million). For zero downtime run two
  replicas behind the proxy/gateway and deploy them one after the other.

## Local check of the deploy files

```bash
shellcheck deploy/remote-deploy.sh
(cd deploy && touch .env && FPX_IMAGE=test docker compose -f docker-compose.prod.yml config -q)
```

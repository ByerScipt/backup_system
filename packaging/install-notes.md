# Backup Studio installation notes

Replace `<version>` with the release number, for example `1.8.0`. Every
artifact is built from the tagged commit recorded in `RELEASE-MANIFEST.txt`;
`SHA256SUMS` covers all of them.

| Artifact | Contents | Intended use |
| :--- | :--- | :--- |
| `backup-studio-client_<version>_amd64.deb` | `backup-gui`, `backup-cli`, desktop entry, icon | Desktop machine that talks to a server |
| `backup-studio-server_<version>_amd64.deb` | `backup-server`, systemd unit, `/etc/backup-studio/databackup.conf` | Dedicated server host, listens on `0.0.0.0` |
| `backup-studio_<version>_amd64.deb` | Everything above plus a loopback drop-in | Single machine, installer provides both sides |
| `backup-studio-<version>-ubuntu24.04-x86_64.tar.gz` | Same files as the all-in-one deb, unpacked layout | Systems without dpkg; extract below `/` |
| `backup-studio-server-<version>-image.tar` | Container image, produced when the release ran with `WITH_DOCKER=1` | `docker load` then run as below |

The packages are built on Ubuntu 24.04 (glibc 2.39) and target Ubuntu 24.04 or
newer. Qt 6, the platform plugin and a CJK font are declared as Debian
dependencies, so `apt-get install ./file.deb` resolves them; `dpkg -i` alone
does not.

## Single machine: all-in-one

```bash
sudo apt-get install ./backup-studio_<version>_amd64.deb
backup-gui                       # desktop client
systemctl status backup-studio   # local server, enabled by the installer
```

The all-in-one package installs a systemd drop-in that starts the server with
`--bind 127.0.0.1`. The NBKP protocol has no TLS, so the archive server must
not be reachable from the network by default. To turn this machine into a
shared server, remove the drop-in, set `bind_address = 0.0.0.0` in
`/etc/backup-studio/databackup.conf` and restart the unit:

```bash
sudo rm /lib/systemd/system/backup-studio.service.d/local.conf
sudo systemctl daemon-reload
sudo systemctl restart backup-studio
```

## Separated deployment

On the server host:

```bash
sudo apt-get install ./backup-studio-server_<version>_amd64.deb
sudoedit /etc/backup-studio/databackup.conf   # port, limits, bind address
sudo systemctl restart backup-studio
```

Archives and accounts live in `/var/lib/backup-studio`, owned by the
`backup-studio` system account. Removing or purging the package never deletes
that directory.

On each client machine:

```bash
sudo apt-get install ./backup-studio-client_<version>_amd64.deb
backup-gui        # remote pages point at 127.0.0.1 by default; change host/port
```

Command-line clients can work without the desktop package's Qt runtime, but
the client package is what carries the CLI; it also installs `backup-cli` for
scripted uploads and restores.

## Containers

```bash
docker load -i backup-studio-server-<version>-image.tar
docker run -d --name backup-studio-server -p 8848:8848 \
    -v backup-data:/data backup-studio-server:<version>
```

`docker-compose.yml` in the repository shows the same service with a named
volume. The container runs as an unprivileged user and stores everything under
`/data`. Mount a configuration over
`/etc/backup-studio/databackup.conf` to change the port or the connection
limit; `storage_path` must stay under `/data`.

## Windows through WSL2

Native Windows builds are not supported. `packaging/windows/install-wsl.ps1`
prepares an Ubuntu 24.04 distribution inside WSL2, installs the all-in-one
package there and creates Start Menu shortcuts that run the Qt client through
WSLg. See the script's header for the prerequisites and for the uninstall
step.

## Upgrades and removal

```bash
sudo apt-get install ./backup-studio_<newer>_amd64.deb   # upgrade in place
sudo apt-get remove backup-studio                        # keep the data
sudo apt-get purge backup-studio                         # keep the data too
sudo rm -rf /var/lib/backup-studio                       # delete the data
```

`/etc/backup-studio/databackup.conf` is a conffile: local edits survive
upgrades and are reported instead of being overwritten.

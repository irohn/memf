# memf

`memf` is a small C utility for multi-event mice. It grabs the physical event devices for one mouse and forwards their mouse events through a single virtual uinput mouse.

## Build

```sh
make
```

## Run

```sh
make run
```

or:

```sh
sudo ./memf
```

## Install

Installs to `/usr/local/bin` by default:

```sh
sudo make install
```

Override the install directory with `INSTALL_PATH`:

```sh
sudo make install INSTALL_PATH=/opt/bin
```

## Systemd Service

Install the binary, config, and service:

```sh
sudo make install-service
```

Enable and start it:

```sh
sudo systemctl enable --now memf.service
```

Check logs:

```sh
journalctl -u memf.service -b --no-pager
```

Uninstall the service:

```sh
sudo make uninstall-service
```

## Options

```sh
sudo ./memf --list
sudo ./memf --debug
sudo ./memf --vendor 09da --product 55c6
./memf --version
```

## Releases

GitHub Actions builds the project on pushes and pull requests.

To publish a release, run the `Release` workflow manually from GitHub Actions. It starts at `v0.1.0`, then increments the selected semver component, commits `VERSION`, tags the release, and uploads Linux x86_64 and ARM64 tarballs containing:

- `memf`
- `systemd/memf.service`
- `memf.conf.example`
- `LICENSE`
- `README.md`
- `VERSION`

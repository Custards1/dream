# ship

`ship` makes a program and its files into what each platform installs:

| Command | Makes | Installed with |
|---|---|---|
| `ship deb` | `NAME_VERSION-REL_ARCH.deb` | `apt install ./x.deb`, `dpkg -i` (Debian, Ubuntu, Mint, ...) |
| `ship rpm` | `NAME-VERSION-REL.ARCH.rpm` | `dnf install ./x.rpm`, `zypper`, `rpm -i` (Fedora, RHEL, openSUSE, ...) |
| `ship arch` | `NAME-VERSION-REL-ARCH.pkg.tar.zst` | `pacman -U` (Arch, Manjaro, ...) |
| `ship tarball` | `NAME-VERSION-OS-ARCH.tar.gz` | unpacking it, or the three below |
| `ship brew` | `NAME.rb`, a Homebrew formula | `brew install` from a tap, macOS and Linux |
| `ship pkgbuild` | `aur/PKGBUILD`, `aur/.SRCINFO` | publishing `NAME-bin` on the AUR |
| `ship installer` | `install.sh` | `curl -fsSL .../install.sh \| sh` |
| `ship sums` | `SHA256SUMS` | `sha256sum -c SHA256SUMS` |
| `ship all` | all of the above that this machine can make | |

Every package is written by `ship` itself, including the tar, ar and cpio
inside it, RPM's binary headers and signature, Debian's control files and
pacman's `.MTREE`. Making a .deb needs no dpkg and making an RPM needs no
rpmbuild, so any machine with a Dream VM can make all of them. The only
programs it runs are the compressors (`gzip`, `xz`, `zstd`), which every
system already has.

The same inputs make the same bytes. Every member of every archive is owned by
root and stamped with one time: `SOURCE_DATE_EPOCH` if it is set, otherwise the
newest source file's. Each compressor is run in the mode whose output depends
only on its input. So a checksum published for a release stays true when
someone rebuilds that release.

## Starting

```
ship init          # adds a [ship] section to mind.toml, filled in from [package]
ship check         # reads it and every file it names, and says what is wrong
ship files         # what every package will install, and from where
ship all           # everything, into dist/
```

`ship` reads `mind.toml` in the current directory (`-m PATH` names another),
so a Dream project describes itself in one place. It takes the name and
version from `[package]` when `[ship]` does not give them.

## The manifest

```toml
[ship]
summary = "a greeting, packaged every way there is"   # one line; required
description = "Hello prints a greeting.\n\nIt exists to be packaged."
maintainer = "Ada Lovelace <ada@example.org>"         # Debian requires it
license = "MIT"                                        # SPDX, as every format wants it
homepage = "https://example.org/hello"
url = "https://github.com/OWNER/REPO/releases/download/v{version}/{file}"
arch = "native"        # or x86_64 / amd64, aarch64 / arm64, all / any / noarch
release = 1            # the package's own revision: 1.2.0-1, 1.2.0-2, ...
prefix = "/usr"        # where a relative destination goes

[ship.files]           # where it goes = what it is made from
"bin/hello" = "cmd/hello"
"share/hello" = { src = "data", exclude = ["target", "*.o"] }
"/etc/hello/hello.conf" = "hello.conf"
"lib/hello/plugin" = { src = "build/plugin", mode = "755" }

[ship.links]           # symbolic links, target as written
"bin/hi" = "hello"

[ship.scripts]         # run by the package manager, as /bin/sh
postinst = "packaging/postinst.sh"

[ship.deb]
depends = ["libc6 >= 2.31", "libssl3t64 | libssl3"]
section = "utils"

[ship.rpm]
depends = ["openssl-libs"]

[ship.arch]
depends = ["openssl"]

[ship.brew]
depends = ["openssl@3"]
test = "assert_match \"hello\", shell_output(\"#{bin}/hello\")"
```

### `[ship]`

| Key | Default | |
|---|---|---|
| `name` | `[package] name` | lower case, digits and `+-.` — the narrowest rule of the formats, Debian's |
| `version` | `[package] version` | starts with a digit and has no `-`; write a pre-release as `1.0~rc1` |
| `summary` | `[package] description` | **required**; one line |
| `description` | the summary | as many paragraphs as needed, with `\n` between lines |
| `maintainer` | | `Name <email>`; **required for a .deb** |
| `license`, `homepage` | | packages say `unknown` without a license |
| `arch` | `native` | `all` for a package with no native code, such as a Dream image |
| `release` | `1` | |
| `prefix` | `/usr` | |
| `url` | | where tarballs will be published, with `{file}` for each one's name; `brew`, `pkgbuild` and `installer` need it |

A template may also name `{name}`, `{version}`, `{release}`, `{os}` and `{arch}`.
`--package-version`, `--release`, `--arch` and `--os` override the manifest
for one run, as a CI job usually wants.

### `[ship.files]` and `[ship.links]`

A **relative destination** goes under the prefix: `bin/hello` is
`/usr/bin/hello` in a .deb, an RPM or an Arch package, and `bin/hello` in a
tarball or a Homebrew keg. An **absolute destination** (`/etc/...`) goes where
it says. A destination with `..` in it is refused.

A **source** is relative to the manifest. A directory ships everything under
it, except what its `exclude` patterns name and what version control keeps
there (`.git`, `.hg`, `.svn`, `.DS_Store`). Patterns are read the way
`.gitignore` reads them. A pattern without a `/` matches a name at any depth:
`target` drops every directory called that, `*.o` every object file. A pattern
with a `/` is anchored at the directory being shipped. `*` and `?` match
within one name, and `**` matches any number of whole names.

A **mode** is octal (`"755"`, `"0o644"`). When the manifest gives none, `ship`
works one out. A file in a `bin`, `sbin` or `libexec` directory is
executable, and so are a script (`#!`) and a native program (ELF or Mach-O).
A shared library (`.so`, `.so.N`, `.dylib`) and everything else are not.

Anything under `/etc`, and any entry marked `config = true`, is
**configuration**: dpkg and pacman keep a copy the user has edited, and RPM
marks it `%config(noreplace)`.

Either table may be written again for one system, as `[ship.files.linux]` or
`[ship.links.macos]`. Those entries are added only when packaging for that
system, because a library is `libx.so.1` on one and `libx.1.dylib` on the
other.

### `[ship.scripts]`

There are four hooks, named as Debian names them: `preinst`, `postinst`,
`prerm` and `postrm`. Each format runs a hook at its own equivalent point. RPM
runs them as `%pre`, `%post`, `%preun` and `%postun`. In an Arch package,
`postinst` becomes both `post_install` and `post_upgrade`, because Debian runs
it after an upgrade too. Each format passes its own arguments, so a script
that has to work everywhere should not read them.

### Each format's own section

Anything a format reads is looked up in `[ship.FORMAT]` first and then in
`[ship]`, where `FORMAT` is `deb`, `rpm`, `arch`, `brew`, `aur` or
`installer`. So `depends` can be written once for every format, and written
again for the format whose packages have different names, which is nearly
always needed.

| | Keys |
|---|---|
| `deb` | `depends`, `pre-depends`, `recommends`, `suggests`, `breaks`, `conflicts`, `provides`, `replaces`, `section` (`misc`), `priority` (`optional`) |
| `rpm` | `depends`, `provides`, `conflicts`, `replaces` (written as Obsoletes), `group` |
| `arch` | `depends`, `recommends` (as optdepends), `conflicts`, `provides`, `replaces`; also used by `pkgbuild` |
| `brew` | `depends` (formula names), `test` (the body of the test block, in Ruby), `caveats`, `url` |

A dependency is written `name`, `name >= 1.2`, or as Debian writes it,
`name (>= 1.2)`. Each format gets its own spelling: `name (>= 1.2)`, RPM's
three header columns, `name>=1.2`, and `depends_on "name"` (formulas carry no
versions). A Debian alternative (`a | b`) is passed to Debian as written.

## Formats

**deb.** An `ar` holding `debian-binary`, `control.tar.xz` and `data.tar.xz`
(`--compress` chooses xz, gzip, zstd or none; zstd needs dpkg 1.21.18). The
control file comes with `md5sums` and `conffiles`. Both tars are in GNU
format, because dpkg unpacks with a tar reader of its own that rejects the
pax records every other format here uses for long names.

**rpm.** A version 3 lead, a signature header holding SHA-256 and SHA-1 of
the header and MD5 of header and payload, the header, and a cpio payload
compressed with xz (or zstd or gzip). The header records SHA-256 digests of
every file and of the payload, which is what `rpm -K` checks. The package owns
the directories it creates but not the system's (`/usr/bin`), so `rpm -e`
removes them. It is unsigned; `rpmsign --addsign` signs it like any other RPM.

**arch.** A zstd (or xz or gzip) tar holding `.PKGINFO`, `.INSTALL` for the
scripts, and `.MTREE` with every file's SHA-256, which is what
`pacman -Qkk` checks against.

**tarball.** The package laid out from the prefix, under one directory named
`NAME-VERSION`. `/etc` and other files outside the prefix keep their paths
from the root. A package whose `arch = "all"` is `NAME-VERSION.tar.gz`, since
it runs anywhere.

**brew, pkgbuild, installer.** These say "download this, and check it is
this", so they are made from the `.tar.gz` tarballs already in the output
directory and never write a checksum down by hand. The formula gets an `on_macos`/`on_linux`
block and an `on_arm`/`on_intel` block for each tarball it finds. The
PKGBUILD is named `NAME-bin`, as the AUR names prebuilt packages, and comes
with a `.SRCINFO` identical to what `makepkg --printsrcinfo` writes. The
install script picks the tarball for `uname`, checks its SHA-256 before
unpacking anything, and installs into `$PREFIX`, or `/usr/local` as root, or
`~/.local` otherwise. Given a tarball already downloaded as its argument, it
checks that one the same way.

## A release on several machines

A tarball made on Linux will not run on macOS, so a release is made in two
steps:

```
# on each machine (CI: one job per platform)
ship tarball                 # and ship deb / rpm / arch on Linux
# then, in one place, with every machine's dist/ gathered into one
ship brew && ship pkgbuild && ship installer && ship sums
```

Dream's own release does exactly this (`.github/workflows/release.yml`).

## Checking it

```
just test-ship
```

That runs ship's unit tests, then `tests/formats.sh`, which makes every
format from `tests/fixture` twice and checks the two runs are identical. It
then hands each package to the program that installs it: dpkg installs,
verifies and purges the .deb; rpm checks the digests, installs, verifies and
removes the RPM; pacman installs and `-Qkk`s the Arch package; makepkg builds
the PKGBUILD; ruby parses the formula; shellcheck and a real install check
`install.sh`. A check whose tool is missing is skipped, and the output says
so. `nix-shell -p dpkg rpm pacman fakeroot ruby shellcheck` has all of them.

## Limits

- `std.io` reports no modes and no links. Modes are worked out as described
  above, and a symbolic link inside a source directory is shipped as the file
  it points to. Declare links in `[ship.links]` instead.
- A .deb takes files under 8 GiB and an RPM under 4 GiB. Each refuses
  anything larger rather than writing the 64-bit forms.
- Packages are unsigned. Sign them with the platform's own tool (`debsigs`,
  `rpmsign`, `gpg --detach-sign` for pacman). Homebrew and the install script
  rely on the SHA-256 they carry.
- No Windows formats yet: an MSI or a Scoop manifest would be the next ones.

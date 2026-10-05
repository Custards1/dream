#!/bin/sh
# ship's packages, checked by the programs that install them.
#
# ship writes every format itself, so its own tests can only say that it
# wrote what it meant to. Whether that is a package is dpkg's question, and
# rpm's, and pacman's -- and they disagree with each other in ways no reading
# of a specification predicts: dpkg's own tar reader refuses the pax records
# `dpkg-deb --contents` lists without complaint. So each package made from
# tests/fixture is handed to the real tool, installed into a scratch root,
# verified and removed, wherever this machine has the tool, and the check is
# skipped, saying so, where it has not. Under nix, the repository's shell
# has none of them; `nix-shell -p dpkg rpm pacman fakeroot ruby shellcheck`
# has all of them.
#
#   dream=build-dream/bin/dream dreams=build/dreams.dream ship/tests/formats.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dream=${dream:-$root/build-dream/bin/dream}
dreams=${dreams:-$root/build/dreams.dream}
case $dream in /*) ;; *) dream=$root/$dream ;; esac
case $dreams in /*) ;; *) dreams=$root/$dreams ;; esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

failed=0
pass() { echo "  ok    $1"; }
skip() { echo "  skip  $1"; }
fail() { echo "  FAIL  $1"; failed=1; }
has() { command -v "$1" >/dev/null 2>&1; }

timeout 300 "$dream" "$dreams" -L "$root/mind" "$root/ship/main.dr" -o "$tmp/ship.dream" >/dev/null
ship() { (cd "$here/fixture" && timeout 120 "$dream" "$tmp/ship.dream" "$@"); }

# One time for every member, so two runs can be compared byte for byte.
export SOURCE_DATE_EPOCH=1700000000

echo "ship: every format, twice"
ship all -q -o "$tmp/a" >/dev/null
ship all -q -o "$tmp/b" >/dev/null
for f in hello_1.2.0-2_all.deb hello-1.2.0-2.noarch.rpm hello-1.2.0.tar.gz hello.rb install.sh SHA256SUMS aur/PKGBUILD aur/.SRCINFO; do
    if [ -f "$tmp/a/$f" ]; then pass "made $f"; else fail "made $f"; fi
done
arch_pkg=$(cd "$tmp/a" && ls hello-1.2.0-2-any.pkg.tar.* 2>/dev/null | head -n 1)
if [ -n "$arch_pkg" ]; then pass "made $arch_pkg"; else fail "made an Arch package"; fi
if diff -r "$tmp/a" "$tmp/b" >/dev/null; then pass "the same inputs make the same bytes"; else fail "two runs differ"; fi

# A namespace in which this user is root, for the installers that will not
# run as anyone else; without one, their installs are skipped.
userns=false
if has unshare && unshare -r true 2>/dev/null; then userns=true; fi

echo "ship: dpkg"
deb=$tmp/a/hello_1.2.0-2_all.deb
if has dpkg-deb && has dpkg; then
    dpkg-deb --info "$deb" >/dev/null && pass "dpkg-deb reads it" || fail "dpkg-deb --info"
    r=$tmp/deb-root
    mkdir -p "$r/var/lib/dpkg/updates" "$r/var/lib/dpkg/info" && touch "$r/var/lib/dpkg/status"
    d="dpkg --force-not-root --force-script-chrootless --root=$r --admindir=$r/var/lib/dpkg --log=$r/log"
    if $d --force-depends -i "$deb" >"$tmp/dpkg.out" 2>&1 && grep -q "hello is installed" "$tmp/dpkg.out"; then
        pass "dpkg installs it, and runs its postinst"
    else fail "dpkg -i: $(tail -n 3 "$tmp/dpkg.out")"; fi
    [ -L "$r/usr/bin/hi" ] && [ -x "$r/usr/bin/hello" ] && pass "the link and the program are there" || fail "installed files"
    $d --verify hello >/dev/null 2>&1 && pass "dpkg --verify finds nothing changed" || fail "dpkg --verify"
    $d -P hello >/dev/null 2>&1 && [ ! -e "$r/usr/share/hello" ] && pass "dpkg purges it" || fail "dpkg -P"
else skip "dpkg is not installed"; fi

echo "ship: rpm"
rpmf=$tmp/a/hello-1.2.0-2.noarch.rpm
if has rpm; then
    out=$(rpm -K -v "$rpmf" 2>/dev/null)
    for digest in "Header SHA256 digest: OK" "Header SHA1 digest: OK" "Payload SHA256 digest: OK" "MD5 digest: OK"; do
        case $out in *"$digest"*) pass "rpm -K: $digest" ;; *) fail "rpm -K: $digest" ;; esac
    done
    [ "$(rpm -qp --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' "$rpmf" 2>/dev/null)" = hello-1.2.0-2.noarch ] \
        && pass "rpm reads its name" || fail "rpm -qp"
    if $userns; then
        r=$tmp/rpm-root
        mkdir -p "$r"
        if unshare -r sh -c "rpm --root '$r' --dbpath /db --initdb && rpm --root '$r' --dbpath /db -i --nodeps --noscripts '$rpmf'" 2>"$tmp/rpm.out"; then
            pass "rpm installs it"
        else fail "rpm -i: $(tail -n 3 "$tmp/rpm.out")"; fi
        # The fixture needs /bin/sh, which the empty root has not got; any
        # other line rpm -V prints is a file that is not what the header says.
        bad=$(unshare -r rpm --root "$r" --dbpath /db -V hello 2>&1 | grep -v -e "Unsatisfied dependencies" -e "is needed by" || true)
        [ -z "$bad" ] && pass "rpm -V finds every file as recorded" || fail "rpm -V: $bad"
        unshare -r rpm --root "$r" --dbpath /db -e --nodeps --noscripts hello 2>/dev/null && [ ! -e "$r/usr/share/hello" ] \
            && [ -d "$r/usr/bin" ] && pass "rpm removes it, and leaves /usr/bin" || fail "rpm -e"
    else skip "rpm install: no user namespace to be root in"; fi
else skip "rpm is not installed"; fi

echo "ship: pacman"
if has pacman && [ -n "$arch_pkg" ]; then
    if $userns; then
        r=$tmp/pacman-root
        mkdir -p "$r/var/lib/pacman" "$r/cache"
        printf '[options]\nArchitecture = auto\nSigLevel = Never\nLocalFileSigLevel = Never\n' >"$tmp/pacman.conf"
        p="pacman --config $tmp/pacman.conf --root $r --dbpath $r/var/lib/pacman --cachedir $r/cache --noconfirm"
        unshare -r $p -dd --noscriptlet -U "$tmp/a/$arch_pkg" >"$tmp/pacman.out" 2>&1 && pass "pacman installs it" \
            || fail "pacman -U: $(tail -n 3 "$tmp/pacman.out")"
        unshare -r $p -Qkk hello 2>/dev/null | grep -q "0 altered files" && pass "pacman -Qkk checks it against .MTREE" \
            || fail "pacman -Qkk"
        unshare -r $p --noscriptlet -R hello >/dev/null 2>&1 && [ ! -e "$r/usr/share/hello" ] && pass "pacman removes it" \
            || fail "pacman -R"
    else skip "pacman install: no user namespace to be root in"; fi
else skip "pacman is not installed"; fi

echo "ship: the AUR"
if has makepkg; then
    conf=${MAKEPKG_CONF:-}
    [ -z "$conf" ] && [ -f /etc/makepkg.conf ] && conf=/etc/makepkg.conf
    [ -z "$conf" ] && conf=$(dirname "$(dirname "$(command -v makepkg)")")/etc/makepkg.conf
    (cd "$tmp/a/aur" && MAKEPKG_CONF=$conf makepkg --printsrcinfo 2>/dev/null) | diff - "$tmp/a/aur/.SRCINFO" >/dev/null \
        && pass ".SRCINFO is what makepkg --printsrcinfo says" || fail ".SRCINFO differs from makepkg's"
    if has fakeroot; then
        cp -r "$tmp/a/aur" "$tmp/aur" && cp "$tmp/a/hello-1.2.0.tar.gz" "$tmp/aur/"
        (cd "$tmp/aur" && MAKEPKG_CONF=$conf PKGDEST=$tmp/aur makepkg -d --noconfirm >"$tmp/makepkg.out" 2>&1) \
            && pass "makepkg builds the PKGBUILD, checksum and all" || fail "makepkg: $(tail -n 3 "$tmp/makepkg.out")"
    else skip "makepkg build: fakeroot is not installed"; fi
else skip "makepkg is not installed"; fi

echo "ship: Homebrew"
if has ruby; then
    ruby -c "$tmp/a/hello.rb" >/dev/null 2>&1 && pass "the formula is Ruby" || fail "ruby -c"
else skip "ruby is not installed"; fi

echo "ship: install.sh"
if has shellcheck; then
    shellcheck -s sh "$tmp/a/install.sh" && pass "shellcheck has nothing to say" || fail "shellcheck"
else skip "shellcheck is not installed"; fi
PREFIX=$tmp/prefix sh "$tmp/a/install.sh" "$tmp/a/hello-1.2.0.tar.gz" >/dev/null && [ "$("$tmp/prefix/bin/hi")" = "hello from the fixture" ] \
    && pass "it installs the tarball, and the program runs" || fail "install.sh"
cp "$tmp/a/hello-1.2.0.tar.gz" "$tmp/bad.tar.gz" && printf x >>"$tmp/bad.tar.gz"
if PREFIX=$tmp/prefix2 sh "$tmp/a/install.sh" "$tmp/bad.tar.gz" >/dev/null 2>&1 || [ -e "$tmp/prefix2" ]; then
    fail "install.sh took a tarball that is not the one it was made for"
else pass "it refuses a tarball that is not the one it was made for"; fi

echo "ship: SHA256SUMS"
if has sha256sum; then
    (cd "$tmp/a" && sha256sum -c --quiet SHA256SUMS) && pass "sha256sum -c agrees" || fail "sha256sum -c"
else skip "sha256sum is not installed"; fi

if [ "$failed" = 0 ]; then echo "every package checks out"; else echo "some packages did not check out"; exit 1; fi

#!/usr/bin/env bash

export LC_ALL=C
TOPDIR=${TOPDIR:-$(git rev-parse --show-toplevel)}
BUILDDIR=${BUILDDIR:-$TOPDIR}

BINDIR=${BINDIR:-$BUILDDIR/src}
MANDIR=${MANDIR:-$TOPDIR/doc/man}

XPCHAIND=${XPCHAIND:-$BINDIR/xpchaind}
XPCHAINCLI=${XPCHAINCLI:-$BINDIR/xpchain-cli}
XPCHAINTX=${XPCHAINTX:-$BINDIR/xpchain-tx}
XPCHAINQT=${XPCHAINQT:-$BINDIR/qt/xpchain-qt}

[ ! -x $XPCHAIND ] && echo "$XPCHAIND not found or not executable." && exit 1

# The autodetected version git tag can screw up manpage output a little bit
XPCVER=($($XPCHAINCLI --version | head -n1 | awk -F'[ -]' '{ print $6, $7 }'))

# Create a footer file with copyright content.
# This gets autodetected fine for xpchaind if --version-string is not set,
# but has different outcomes for xpchain-qt and xpchain-cli.
echo "[COPYRIGHT]" > footer.h2m
$XPCHAIND --version | sed -n '1!p' >> footer.h2m

for cmd in $XPCHAIND $XPCHAINCLI $XPCHAINTX $XPCHAINQT; do
  cmdname="${cmd##*/}"
  help2man -N --version-string=${XPCVER[0]} --include=footer.h2m -o ${MANDIR}/${cmdname}.1 ${cmd}
  sed -i "s/\\\-${XPCVER[1]}//g" ${MANDIR}/${cmdname}.1
done

rm -f footer.h2m

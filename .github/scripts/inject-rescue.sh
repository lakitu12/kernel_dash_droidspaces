#!/usr/bin/env bash
# Inject the trusty-rescue driver + nvhe CONFIG_CMDLINE into the synced GKI tree.
# Called from CI between defconfig-patch and build. KDIR layout identical to
# the pid_max clamp step.
set -euo pipefail
cd ~/gki
if [ -d common ]; then KDIR=common; elif [ -d kernel/common ]; then KDIR=kernel/common; else
  KDIR=$(find . -name "Kconfig" -path "*/arch/arm64/*" | head -n1 | xargs dirname | xargs dirname | xargs dirname)
fi
echo "KDIR=$KDIR"

# ---------- 1) copy driver ----------
mkdir -p "$KDIR/drivers/trusty_rescue"
cp "$GITHUB_WORKSPACE/rescue/trusty_rescue.c" "$KDIR/drivers/trusty_rescue/trusty_rescue.c"
cp "$GITHUB_WORKSPACE/rescue/Makefile"        "$KDIR/drivers/trusty_rescue/Makefile"
cp "$GITHUB_WORKSPACE/rescue/Kconfig"         "$KDIR/drivers/trusty_rescue/Kconfig"

# ---------- 2) hook into drivers/Kconfig (before final endmenu) ----------
if ! grep -q 'drivers/trusty_rescue' "$KDIR/drivers/Kconfig"; then
  perl -0777 -i -pe 's/(source "drivers\/cdx\/Kconfig")/$1\n\nsource "drivers\/trusty_rescue\/Kconfig"\n/' "$KDIR/drivers/Kconfig"
  grep -q 'trusty_rescue' "$KDIR/drivers/Kconfig" || { echo "::error::Kconfig hook failed"; exit 1; }
fi
echo "drivers/Kconfig hooked:"; tail -4 "$KDIR/drivers/Kconfig"

# ---------- 3) hook into drivers/Makefile ----------
if ! grep -q 'trusty_rescue' "$KDIR/drivers/Makefile"; then
  printf '\nobj-$(CONFIG_TRUSTY_RESCUE) += trusty_rescue/\n' >> "$KDIR/drivers/Makefile"
fi
tail -3 "$KDIR/drivers/Makefile"

# ---------- 4) CONFIG_CMDLINE: protected -> nvhe ----------
# Field truth: the boot.img cmdline our byte-patched kernels carry ends up with
# kvm-arm.mode=nvhe in /proc/cmdline; the built-in CONFIG_CMDLINE is its source.
# gz-intact boots run the kernel at EL1 where KVM init prints "KVM is not
# available. Ignoring kvm-arm.mode" -> the string is harmless there.
DEF="$KDIR/arch/arm64/configs/gki_defconfig"
if ! grep -q 'kvm-arm.mode=protected' "$DEF"; then
  echo "::error::CONFIG_CMDLINE kvm-arm.mode=protected not found in $DEF - upstream moved?"
  exit 1
fi
sed -i 's/kvm-arm\.mode=protected/kvm-arm.mode=nvhe/' "$DEF"
if ! grep -q 'kvm-arm.mode=nvhe' "$DEF"; then
  echo "::error::cmdline flip failed"; exit 1
fi
if grep -q 'kvm-arm.mode=protected' "$DEF"; then
  echo "::error::protected string still present after flip"; exit 1
fi
echo "CONFIG_CMDLINE now:"; grep 'CONFIG_CMDLINE=' "$DEF"

# ---------- 5) defconfig enable (ensure_y in the other step is not shared here) ----------
grep -q '^CONFIG_TRUSTY_RESCUE=y' "$DEF" || echo 'CONFIG_TRUSTY_RESCUE=y' >> "$DEF"
echo "trust_rescue injection complete"

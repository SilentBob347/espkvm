#!/usr/bin/env bash
# Build the Wi-Fi co-processor image that the firmware carries and can install
# into the board's ESP32-C6 (the network popup in the console: "Wi-Fi chip").
#
#   tools/build-coproc.sh esp32c6
#
# The C5 is left out for now: older C5 builds keep their partition table at
# 0xc000, and this example is built for 0x8000 (see KVM_WIFI_COPROC_UPDATE).
#
# It builds esp-hosted's own co-processor example from the same esp_hosted
# component the firmware uses as the host, so both sides run one version. The
# result goes to components/kvm_net/coproc/<target>.bin and is committed: a
# board build then needs no second toolchain run. Rebuild it after every
# esp_hosted bump (the firmware warns at build time when they differ).
#
# Needs the ESP-IDF environment:  . tools/env.sh
set -euo pipefail

target="${1:?usage: $0 esp32c6|esp32c5}"
here=$(cd "$(dirname "$0")/.." && pwd)
hosted="$here/managed_components/espressif__esp_hosted"
[ -d "$hosted" ] || { echo "No $hosted - build any Wi-Fi board once first." >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$hosted/examples/ota/coprocessor_ota/cp" "$work/cp"
cat > "$work/cp/main/idf_component.yml" <<EOF
dependencies:
  espressif/esp_hosted:
    version: '*'
    override_path: $hosted
  idf:
    version: '>=5.5'
EOF

cd "$work/cp"
idf.py set-target "$target" >/dev/null
idf.py build >/dev/null

out="$here/components/kvm_net/coproc/$target.bin"
mkdir -p "$(dirname "$out")"
cp build/eh_cp_ota_coprocessor_ota.bin "$out"
ver=$(grep -m1 '^version:' "$hosted/idf_component.yml" | awk '{print $2}' | tr -d "'\"")
echo "$ver" > "${out%.bin}.version"
printf '%s: %s bytes, esp_hosted %s\n' "$out" "$(wc -c < "$out")" "$ver"

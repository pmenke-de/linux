#!/bin/sh
#
# mdm-gpio-monitor - continuously record SDX55 ESOC sideband GPIO state
#
# Writes one line every 250ms to /var/log/mdm-trace/gpio.log:
#   <monotonic_sec> ap2mdm_status ap2mdm_errfatal mdm2ap_status mdm2ap_errfatal
#
# Reads from /sys/kernel/debug/gpio because the four ESOC sideband lines
# are owned by the kernel qcom,mdm0 driver — userspace gpioget would fail
# with EBUSY, but the debugfs snapshot reflects the live pin state.
#
# debugfs format (sm8250 tlmm):
#   gpiochip3: 181 GPIOs, parent: platform/f100000.pinctrl, f100000.pinctrl:
#    gpio56  : out high func0 16mA no pull
#    gpio57  : out low  func0 16mA no pull
# Pin numbers are chip-relative tlmm offsets, values are "high"/"low".

LOG=/var/log/mdm-trace/gpio.log
DBG=/sys/kernel/debug/gpio
mkdir -p "$(dirname "$LOG")"

if [ ! -r "$DBG" ]; then
    echo "fatal: $DBG not readable (need root + CONFIG_DEBUG_FS)" >&2
    exit 1
fi

# Apollo SDX55 ESOC sideband pins (tlmm offsets):
#   1  = MDM2AP_ERRFATAL  (in)
#   3  = MDM2AP_STATUS    (in)
#   56 = AP2MDM_STATUS    (out)
#   57 = AP2MDM_ERRFATAL  (out)

{
    echo "# mdm-gpio-monitor start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# fmt: monotonic ap2mdm_status ap2mdm_errfatal mdm2ap_status mdm2ap_errfatal"
} >> "$LOG"

while :; do
    t=$(awk '{print $1}' /proc/uptime)
    out=$(awk '
        # Enter the tlmm chip section, exit on the next gpiochip header.
        /^gpiochip[0-9]+:.*f100000\.pinctrl/ { in_tlmm = 1; next }
        /^gpiochip[0-9]+:/                   { in_tlmm = 0; next }
        in_tlmm && /^[[:space:]]*gpio[0-9]+[[:space:]]*:/ {
            n = $1
            sub(/^gpio/, "", n)
            n = n + 0
            if (n != 1 && n != 3 && n != 56 && n != 57) next
            v = "?"
            for (i = 1; i <= NF; i++) {
                if ($i == "high") v = 1
                else if ($i == "low") v = 0
            }
            vals[n] = v
        }
        END {
            printf "%s %s %s %s",
                (56 in vals ? vals[56] : "?"),
                (57 in vals ? vals[57] : "?"),
                (3  in vals ? vals[3]  : "?"),
                (1  in vals ? vals[1]  : "?")
        }
    ' "$DBG")
    printf "%s %s\n" "$t" "$out" >> "$LOG"
    sync
    usleep 250000 2>/dev/null || sleep 0.25
done

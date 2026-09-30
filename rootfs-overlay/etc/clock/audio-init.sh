#!/bin/sh
# Audio bring-up for the Smart Clock (gen 1, "smini" board), replaying what the stock
# Android Things app did (com.google.android.things.sparrow.oem, Lenovo.enableAmplifier /
# enableMicrophone). No kernel driver touches either chip.
#
#   speaker: PCM hw:0,0 ("I2S8CH Playback") -> TI TAS5805M amp, i2c-2 0x2c (bridged/PBTL)
#   mics:    TI TLV320ADC3101, i2c-1 0x1b, enabled/reset by GPIO24 -> PCM hw:0,1 (TDM_Capture)
#
# Once in Play the amp idles in Hi-Z without an I2S clock and switches back to Play when a
# stream starts, so this runs once per power-on.
set -e
cd /etc/clock

# Write "reg value" lines from a file to an I2C device, 40 writes per i2ctransfer call.
write_regs() { # bus addr file
	bus=$1 addr=$2 args= n=0
	grep -v '^#' "$3" | {
		while read -r reg val; do
			args="$args w2@$addr $reg $val"; n=$((n + 1))
			if [ $n -ge 40 ]; then i2ctransfer -y "$bus" $args; args= n=0; fi
		done
		[ -z "$args" ] || i2ctransfer -y "$bus" $args
	}
}

# Mic ADC: pulse its enable/reset line (Android Things "GPIO24" = SoC gpio 24 = 387 + 24).
G=/sys/class/gpio/gpio411
[ -d $G ] || echo 411 > /sys/class/gpio/export
echo high > $G/direction
echo in > $G/direction
sleep 0.02
echo high > $G/direction
sleep 0.05
write_regs 1 0x1b tlv320adc3101-init.regs

# Amp: the stock code keeps silence playing (I2S clocks running) while it programs the DSP.
aplay -q -D hw:0,0 -f S16_LE -c 2 -r 48000 -d 6 /dev/zero &
SILENCE=$!
sleep 1
write_regs 2 0x2c tas5805m-hiz.regs
sleep 0.5
write_regs 2 0x2c tas5805m-init.regs
sleep 1
kill $SILENCE 2>/dev/null || true
wait $SILENCE 2>/dev/null || true

# fake-mic — virtual Ultramic for LiteSpectrum

A GUI that feeds a WAV into an ALSA loopback so the production LiteSpectrum
binary captures it **exactly as if a Dodotronic Ultramic 384K were plugged in** —
no ESP32, no Pi, no hardware.

## Why this works

LiteSpectrum opens its mic through ALSA (`snd_pcm_open` on whatever `--device`
name you pass). ALSA hides USB completely, so a virtual `snd-aloop` capture
device at 384 kHz / S16_LE / mono is indistinguishable from the real mic to the
app. This tool is just the *producer*: it writes a chosen WAV into the
loopback's playback endpoint, real-time paced (ALSA blocking writes are clocked
by the device), and the app reads the paired capture endpoint.

```
fake-mic GUI  ──write──>  hw:UltraMic384K,0,0  ╮
                                                │  snd-aloop pairs 0<->1
LiteSpectrum  <──read───  plughw:UltraMic384K,1,0  ╯  --device
```

## One-time setup

The loopback module must be loaded once per boot. There's no passwordless sudo
here, so run it yourself (in a Claude session you can prefix with `!`):

```
sudo modprobe snd-aloop id=UltraMic384K
```

The `id=UltraMic384K` makes the virtual card literally named `UltraMic384K`, so
the same `--device plughw:CARD=UltraMic384K` command works here and on the Pi.

To make it survive reboots:

```
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
echo "options snd-aloop id=UltraMic384K" | sudo tee /etc/modprobe.d/snd-aloop.conf
```

Python deps (into the repo venv, no sudo): `pip install -r requirements.txt`.

## Run

```
./fake-mic
```

1. The status line confirms the loopback is loaded and prints the exact
   `LiteSpectrum --device …` command to copy.
2. **Browse** to a 384 kHz / S16 / mono WAV. The format is shown; a warning
   appears if it isn't the production format.
3. **Play** (optionally **Loop** so the mic runs continuously). **Stop** anytime.
4. In another terminal, run the app against the capture endpoint:

   ```
   ./deploy/bin/LiteSpectrum --device plughw:UltraMic384K,1,0
   ```

## Troubleshooting

- **"snd-aloop not loaded"** — run the `modprobe` above, then *Re-check device*.
- **"loaded as 'Loopback', not 'UltraMic384K'"** — you loaded it without `id=`.
  `sudo modprobe -r snd-aloop && sudo modprobe snd-aloop id=UltraMic384K`.
- **Open fails at 384000 Hz** — the stock `snd-aloop` in your kernel is capped at
  192 kHz. 192 kHz puts Nyquist at 96 kHz and throws away most of the bat band,
  so resampling is *not* an acceptable fallback. Fix by rebuilding `snd-aloop`
  with `rate_max = 384000` (out-of-tree / DKMS, one-line change), or switch to a
  custom userspace ALSA capture plugin (no rate cap). Ask and we'll wire it up.

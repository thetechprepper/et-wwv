# et-wwv

et-wwv takes a simple approach to setting the system clock to the next
top-of-the-minute using a live audio stream from WWV. It uses only the
1000 Hz tone to align the clock to the minute boundary. It is not a full
WWV/WWVH decoder and assumes the operator has already set the correct
date, hour, and minute.

## WARNING

Setting the system clock requires elevated privileges and may impact
running services. Use with caution.

## BUILD

```bash
$ ./build.sh
```

This creates the et-wwv binary in `build/et-wwv`.

## BEHAVIOR

- The program listens for the WWV 1000 Hz tone.
- When the tone ends, it sets the system clock to the next minute
  boundary (seconds = 00).
- Processing stops after the first valid tone is detected.

## USAGE

1. Set your radio to one of the WWV/WWVH frequencies: 2.5 MHz, 5 MHz, 10 MHz,
   15 MHz or 20 MHz AM.

2. Determine your ALSA capture device connected to your radio
   (for example: `hw:1,0`)

3. Run et-wwv in ALSA mode (`-a`).

```bash
$ ./build/et-wwv -a --device hw:1,0
```

If you wish to actually set the clock, run the above command with `sudo`.

- If no tone is detected, the program will exit after the configured
  capture duration (default: 120 seconds).

## TESTING

If you have a WWV WAV file recorded, you can analyze it with:

```bash
$ ./build/et-wwv -f sample/wwv-capture.wav
WWV 1000 Hz candidate tones:
1. start=35.360 end=36.170 duration=0.810 expected=0.800
2. start=95.350 end=96.170 duration=0.820 expected=0.800
Stats: interval_count=896 candidate_count=2
```

## NOTES

- Audio should be clean and free of clipping for best results.
- AM mode is recommended for WWV reception.

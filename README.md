# fm95

FM95 is a audio processor for FM, it does:

- Pre-Emphasis
- Low Pass Filtering
- AGC
- Stereo
- BS412 (mpx power limiter, simplest implementation ever)

Supports these inputs:

- Audio (via Pulse)
- MPX (via Pulse, basically passthrough, i don't recommend this unless you have something else than rds or sca to modulate, you could run chimer95 via here, also you have 5% allowed here by default to be guarenteed with no clipping, change how much headroom you have with the headroom option)
- RDS (via Unix Socket, expects RDS bits without differential encoding with checkwords, rds95 is recommended here)

and one output:

- MPX (via Pulse)

## How to compile?

Note that you're required also to load submodules, if you don't know what that means, ask ChatGPT

To compile you need `cmake`, `liquid-dsp` and `libpulse-dev`, if you have those then do these commands:

```bash
mkdir build
cd build
cmake ..
make
```

Done!

## CPU Usage?

Should run completly fine on a pi 5, fine on a pi 3b (~30% cpu)

## Other Apps

FM95 also includes some other apps, such as chimer95 which generates GTS tones each half hour, and vban95 now which is a buffered VBAN receiver. And now also SCA generation was moved to sca95 from fm95!

## Feature Requests

In case you are missing something, you can create an issue, and if you actually do need the feature and can prove it, and also provide/tell how to test the feature - any feature is welcome to be implemented (though i never said when will it be done)

## Usage of other projects

The apps use inih by Ben Hoyt.

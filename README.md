# PSX-Hashcat

This is a "port" (not really) of the HashCat password cracker to the Playstation 1, based on [GBA-HASHCAT](https://github.com/solst-ice/gba-hashcat).

The program streams `data/WORDLIST.TXT` from the CD because the dictionary is larger than the PS1's 2 MB RAM.

## Build

Requires PSn00bSDK 0.24 and Python 3.

```sh
export PSN00BSDK=/path/PSn00bSDK
export PSN00BSDK_LIBS="$PSN00BSDK/lib/libpsn00b"
export PATH="$PSN00BSDK/bin:$PATH"

cmake --preset default
cmake --build build
```

## Editing

- Splash logo: `data/assets/logo.png`
- Splash title: `data/assets/title.png`
- Dictionary: `data/WORDLIST.TXT`
- Target SHA-256 hash: `TARGET_HASH_HEX` in `src/main.c`

After replacing the dictionary, update its generated metadata:

```sh
python3 tools/gen_wordlist_info.py data/WORDLIST.TXT src/wordlist_info.h
```

## Controls

-  **START:** pause or resume
-  **CIRCLE:** abort
-  **SELECT:** restart after completion or an error
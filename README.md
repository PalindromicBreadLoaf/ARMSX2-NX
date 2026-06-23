<div align="center">

![ARMSX2-NX](app_icons/icon.png)

# ARMSX2-NX

[![License](https://img.shields.io/github/license/ARMSX2/ARMSX2)](https://www.gnu.org/licenses/gpl-3.0.html)
(https://patreon.com/ARMSX2)
(https://github.com/sponsors/PCSX2)

</div>

ARMSX2-NX (please help me come up with a better name) is a free and open-source PlayStation 2 emulator for the Nintendo Switch based on [ARMSX2](https://armsx2.net/).

ARMSX2-NX is in no way associated with the ARMSX2 team. Do not contact them regarding any bugs or help with this port of the emulator. They have nothing to do with this project.

The goal here is to prove that PS2 games can run on the Switch, not that they run well. They very much don't.

**NOTE: Do NOT expect playable performance out of this project. The only full-speed thing I've seen run is the PS2 BIOS, load screens, and the MegaMan Legacy Collection.**

## Requirements

You need to have a hacked Nintendo Switch to run this. You also will need to overclock your Switch to get even somewhat acceptable performance. I'm not going to explain how to do that here, that's on you to figure out.

Please note that a BIOS dump from a legitimately-owned PS2 console is required to use the emulator.

## Download

ARMSX2-NX can be downloaded from [Github Releases](https://github.com/PalindromicBreadLoaf/ARMSX2-NX/releases).

## Building
This assumes you have DevkitPro installed. Install it if you don't.

```shell
dkp-pacman -S switch-dev switch-libpng switch-libjpeg-turbo
```

Configure the native core with the Switch toolchain, then build the `.nro` target:

```shell
cmake -S app/src/main/cpp -B build/switch-core \
      -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/switch-core --target armsx2_nro -j$(nproc)
```

The output binary will be located at `build/switch-core/switch-app/armsx2nx.nro`

You will also need the resources from the releases page to properly display the fullscreen UI.

## Affiliation

ARMSX2-NX is NOT affiliated with ARM Holding LTD in any way shape or form. Nor is the project associated with ARMSX2 or PCSX2 or any derivatives of said projects. Please donate to either of these projects before even trying to donate to this.

## Contributing

All contributions to this project are welcome! There's a lot still to be re-implemented that was stubbed out from ARMSX2 and a lot of performance that needs to be gained if possible.

If you don't know/don't want to code, please just open issues for games not rendering properly, app crashes, missing features, or anything else that needs done/fixing.

## Additional Credits

[ARMSX2](https://armsx2.net/) - ARMSX2-NX would never have got anywhere near the performance it (already doesn't) have without the outstanding work of the ARSMX2 team. 

[PCSX2](https://github.com/PCSX2/pcsx2) - ARMSX2 would not be possible without the legendary work from the PCSX2 team and their patience and understanding regarding this project!

[PCSX2_ARM64](https://github.com/pontos2024/PCSX2_ARM64) - ARMSX2 originally started off as a fork of developer Pontos work. 

Thank you to [@fffathur](https://github.com/fffathur) and [@Vivimagic](https://github.com/Vivimagic) for creating and working on the logo! 

# Credits

An unofficial fan port, not affiliated with, sponsored or endorsed by Ubisoft,
Sony Interactive Entertainment or Microsoft. Tom Clancy's Ghost Recon: Future
Soldier is by Ubisoft; all rights to the game belong to Ubisoft. No game files
are distributed: playing needs your own original copy. The PS5 home screen's
icon, background and launch screen (assets/ps5/icon0.png, pic0.dds, pic1.dds)
and the ISO screen's wallpaper (assets/iso_background.png) are made from the game's promotional art, © Ubisoft, used as fan art.

## Recompilation

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Xbox 360 code to
  native C++.
- [Xenia](https://github.com/xenia-project), the Xbox 360 emulator ReXGlue is
  rooted in.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [rexdex's recompiler](https://github.com/rexdex/recompiler), which inspired it.

## PS5

- elripalda: ReXGlue's PS5 layer (rexglue-ps5) and the native PS5 ports
  before this one.
- Nazky: [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) and
  UnleashedRecomp-Prospero, the link and packaging tooling.
- mihawk-99: the PS5 Payload SDK fork, Mesa RADV for the PS5 and its link
  recipe.
- John Törnblom: the original ps5-payload-sdk.

## Libraries and assets

Mesa (RADV), Vulkan, SDL3, Dear ImGui, FFmpeg, glslang and SPIRV-Tools, spdlog,
fmt, toml++, xxHash, stb and the rest of ReXGlue's thirdparty folder (each under
its own license there); AMD FidelityFX FSR; QR Code generator by Project Nayuki
(MIT, src/third_party/qrcodegen); Noto Sans fonts by Google (SIL Open Font
License, assets/metadata/fonts/OFL.txt); extract-xiso.

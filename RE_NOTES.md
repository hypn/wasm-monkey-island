# Monkey Island demo (MIDEMO) — reverse-engineering notes

Target: `mi_demo/` — "The Secret of Monkey Island" contest demo (1991), DOS, EGA/VGA.
Tools: own Python scripts (`work/*.py`), REA (Ghidra 12.1.4 provider) on the unpacked EXE,
Ghidra headless (`work/gh/scripts/MkFuncs.java`) to create/decompile functions that Ghidra's
auto-analysis missed (they are only reachable through a far-pointer table).

## Data files
| file | encoding | content |
|---|---|---|
| `DISK01.LEC` | every byte XOR 0x69 | `LE` container → `FO` (room offset table) + `LF` room files |
| `000.LFL` | block headers plain, payload XOR 0xFF | index: `RN` room names, `0R` room dir, `0S` scripts dir, `0N` sounds dir, `0C` costumes dir, `0O` object dir |
| `901/902/904.LFL` | see charsets (TODO) | charset / font data |

Block header: `u32 LE size (incl. header)`, 2-char tag. `LE`→`FO`(41 bytes: count, then n×{room u8, offset u32}) then `LF` blocks
(`LF` = `u16 room id` + blocks). A room `RO` block holds
`HD`(w,h,numObjects u16 each) `BX`(boxes) `SA`(32 byte table) `BM`(bitmap) `OI`×n (object image) `NL SL`(counts) `OC`×n (object code)
`EX`(exit script) `EN`(entry script) `LC`(local script count) `LS`×n (local scripts); after `RO`: global `SC` scripts, `CO` costumes.

### Room bitmap (BM) — verified by rendering room 2 (`work/r2.png`)
`BM` data: `u16 total`, then `w/8` × `u16` strip offsets (relative to start of BM payload), each strip 8px wide column-major,
EGA RLE (high bit: run / copy-left; 0x40: two-colour alternating nibble run; else nibble count|colour). Standard 16-colour EGA palette.

## EXE
* `MIDEMO.EXE` is LZEXE 0.91 packed (`LZ91` at MZ+0x1C). `work/unlz.py` unpacks → `MIDEMO_UNP.EXE` (112075 bytes image).
  Original entry 0fe8:2ece, ss:sp 21e5:0800. MS C 5.x/6 run-time ("MS Run-Time Library - Copyright (c) 1990").
* DGROUP base = image offset 0x1a130 (startup does `mov di,0x1a13; mov ss,di`). Ghidra linear address = 0x10000 + image offset.
* Script engine: main loop `FUN_1000_5cde` (0x15cde): `while (curSlot != 0xFF){ op = fetchByte(5c8c); opcode=op; call far [DS:0x48c + op*4] }`.
  Evidence ev_de731d4ba47ac21e6554d203ab963c4e90fc664e938c2d0e1351d775d4940ffc.
* **Opcode table**: 256 far pointers at image 0x1a5bc (DS:0x048c), 103 distinct handlers in CODE_0 offsets 0x68f6..0x965c.
  Layout is the SCUMM v4/v5 family (0x18 = jumpRelative: `pc += fetchWord()`, 0x00 = stopObjectCode, 0x80 bit = param-is-variable...).
  Handlers decompiled to `work/gh/handlers.c`.
* Script pc is `DS:6f42/6f44` (off/seg), current slot `DS:66a6`, last opcode `DS:72a5`.

## Engine facts verified from the decompile (see `work/gh/handlers.c`, `work/decomp/functions/*.c`)
* Variable operands: bit 0x8000 = bit variable, 0x4000 = script-local (17 words/slot, 20 slots), 0x2000 = indexed (`FUN_1000_64d4`). Parameter
  "var-or-direct" masks 0x80/0x40/0x20 apply to the **last opcode byte** (`DS:72a5`).
* Boot: `FUN_1000_0000` starts global script 1 with args at DS:0x196e (0 when no command-line argument) then loops:
  wait `VAR_TIMER_NEXT` ticks (60 Hz), `FUN_682a` decrements paused-script delays, run all slots (`FUN_5c50`), sentence dispatcher (`a3a0`,
  LIFO stack), actors (`af10`/`b600` walk + `d18c` draw), camera (`151e`, 8 px/frame), message timer (`9b64`: 60 + `VAR_CHARINC`*chars ticks).
* Object owner/state: one byte per object (`DS:0x3cc6`), state = high nibble, owner = low nibble; class bits = 3 bytes (`DS:0x1be8`).
* Object `OC` header (offsets from block start): +6 id, +9 x/8, +10 y/8 (&0x7f), +11 w/8, +12 parent, +13/+15 walk x/y (s16), +17 height|dir,
  +18 name offset, +19 verb table `{verb u8, off u16}*,0`. `OI` = id + image (`u16 size`, per-strip `u16` offsets; same strip codec as `BM`).
* Room load tags (DS:0x38f..): `HD BX BM EX EN SA`. Entry/exit run `VAR_ENTRY_SCRIPT`, room `EN` (slot number 998), `VAR_ENTRY_SCRIPT2`; exit = 30/`EX`(999)/31.
* Costumes: all offsets relative to block start; header `[6]=numAnim [7]=format(0x58=16 colours) [8..]=palette`, then `u16 animCmds`,
  16×`u16` limb picture tables, `numAnim+1`×`u16` anim definitions. Picture header 12 bytes `w,h,relx,rely,movex,movey` then column RLE (`colour<<4|run`).
* Charset (`901/902/904.LFL`): `u32 size, u16 0x0363, 15-byte colour map, bpp, height, u16 n, u32 offsets[n]` (relative to the bpp byte);
  glyph = `w h xoff yoff` + **continuous** bit stream (rows are not byte aligned).
* Text escapes: `0xFF 1|8` newline, `0xFF 2` keep, `0xFF 3` wait, `0xFF 4..7 <u16>` var/verb/object/string substitution; `@` is dropped.
* Main view starts at y=16, 144 rows (`roomOps 3` = top,height); EGA palette matches the oracle exactly.

## Costume rendering details (verified by decompile + visual checks)
* Animation commands (bytes in the `animCmds` stream, low 7 bits): `0x79` = stop limb, `0x7A` = restart limb, `0x7B` = hide, `0x78`/`0x7C` = counter
  commands (skipped when stepping, `FUN_1000_d272`). **A stopped limb is not drawn** — walk anims stop the head limb (limb 1) because the head is baked into
  limb 0; ignoring this paints the stand-frame head over the walking sprite (the "double head" bug).
* Actor colours: costume palette maps picture index -> EGA colour; `actorOps 13/17` override entries (`FUN_1000_b414`); lights bit 3 off = dark grey actors.
* Mirroring is `FUN_1000_d1d6`; west/east share the same pictures (anim `n*4+dir`).

## Foreground masks (z-order of actors vs objects)
Each `OI` block carries, after the image data (`img + u16 size`), a mask: `u16 total`, `u16 per-strip offset` (relative to the mask start) then per strip a
`decompressMaskImg`-style RLE: byte `b` with bit7 = run of `b&0x7f` rows of the next byte, else `b` literal rows. One byte = 8 pixels (MSB = left) of one row.
Set bits are foreground that hides actors standing in a walkbox whose `mask` byte is non-zero (every box in the bar has mask 1). All-zero masks (`b0 00`) mean "no overlap".
This is why Guybrush must walk behind the bar tables and pirates (which are objects, not actors).

## Verification oracle (dynamic analysis of the original)
`work/oracle/run.sh <n>` copies `mi_demo`, starts `dosbox-x` under `xvfb-run` (`machine=vgaonly`, `midemo e k`), and takes `import -window root` screenshots
(~2.3 s apart). The emulated 320x200 screen sits at (192,201) in the 1024x768 capture at 2x. `work/oracle/*` helper snippets (python/numpy) downsample a shot and
compare against my engine frames:
* palette check: for the headshot scene my 16-colour render is identical to the original (only text timing differed, 99.2% pixel match);
* observed: the original under DOSBox-X dies with `R6003 integer divide by 0` at the end of the intro when `cycles=max`; with `cycles=fixed 12000` it stayed black,
  so it could not be used to check the dock scene (open item).
Native engine tests: `engine/mi_native <dir> <frames> <prefix> <every>` with env `CLICKS=frame:x:y:btn,...`, `GOTO=<room>` (+`EX`,`EY`), `TRACE=1`, `TRACE_FROM=n`,
`TRACE_ROOM=1`, `TRACE_OBJ=id`, `TRACE_VAR=n`, `TRACE_TXT=1`, `DUMP=1`; `work/sheet.py`/`ppm2png.py` build contact sheets. Browser test: `web/test/cdp.js` (CDP over
headless Chrome: real clicks/keys, screenshots, `?speed=N` to run N times faster).

## Fixes after first play-test
* `putActorInRoom(a,0)` also resets x,y to 0 (`FUN_1000_b118(a,0,0,0)`); the ego-restore in `eng_start_room` must restore room **and position** (ego starts at 360,100 off-screen right
  and walks in; with x=0 the dock troll script 203 (reacts to ego x) fired and Guybrush "walked in" from the top left).
* `saveRestoreVerbs` (op 0xAB): saved verbs must not be drawn/hit-tested (the title cutscene hides the verb panel this way).

* `putActor` snaps the actor into the nearest walkbox (ScummVM `adjustXYToBeInBox`); ego put at (360,100) therefore starts at (360,132) and walks flat. Walk destinations are snapped
  the same way, so clicking the ceiling in the bar stops at the box edge. (Careful: WASM `long` is 32-bit — use `long long` for squared distances.)
* Camera follow (`FUN_1000_7e3c`/`151e`): follow only re-centres when the actor is outside strips 10..30 of the view; `actorFollowCamera` no longer jumps the camera.

* **Mirroring**: verified visually that `mirror = (facing == 0) ^ (format & 0x80)` is correct (a decompile-based inversion made Guybrush moonwalk — `FUN_1000_d1d6`'s flag means "not mirrored"). Multi-limb x offsets accumulate `+movex` in both modes.
* **Actor classes** (3 bytes at `DS:0x1be8`, class n = bit n-1): 20 = NeverClip (ignore foreground masks), 21 = AlwaysClip, 22 = IgnoreBoxes (no walkbox snapping; the dock troll is
  placed at x=100 outside any box — snapping it moved it to x=147, "too far right").
* Walking to an object (`op 0x36`) takes the object's `actordir` (OC byte +17 low 3 bits) as the arrival facing; plain `walkActorTo` keeps the travel direction.
* Head/limb direction: when an animation does not cover a limb (e.g. stand anims only `0x7A`-restart the head), every other active limb is re-decoded with its own frame base and the
  new direction (ScummVM `Actor::setDirection`), otherwise the head keeps looking at the screen.

* **Room exits are `actorFollowCamera`**: a door script does `putActorInRoom(ego, room)`, `putActor(ego, x, y)`, `actorFollowCamera(ego)`; `FUN_1000_7e3c` then switches to the
  followed actor's room. Without that the actor just vanished (room != current) and the scene never changed. Doors are state objects: `Open` first, then `Walk to`.
* `loadRoomWithEgo` faces ego `objdir ^ 1` (handler 726e: `animate 0xF8 + (dir^1)`); `findObject` returns the **first** matching object in room order.

* **ceae (start animation)** `FUN_1000_ceae(anim)`: `anim = frame*4+dir`; init frame resets all limbs; frames `0xF8..0xFB` only change direction; otherwise decode the anim and remember `cur_frame`;
  if the direction changes every other active limb is re-decoded for it (`d12e`); finally `facing = anim & 3`. Costume set-up defers the init anim until the actor is shown (`b2e8`/`b1e8`).
* **Walker**: moving flags 1 new-leg / 2 in-leg / 4 turn / 8 last-leg; speed vector from `b842` (zero deltas become 1, ratio clamped to `speedx`); step = `(delta>>8)*scale` in 16.16; facing target vertical when
  `|dx| < 3|dy|`; one turn-table step per frame (`DS:0x892`); box flags &7 force a direction, &8/&0x10 and classes 19/18 flip it. Scale: box word, `0x8000|n` = room scale slot n, per-row table from `d430`.
* Tables extracted from the EXE: `work/gen_tables.py` -> `engine/tables.h`.

## Open questions
* Exit script of room 3 kills actors 1..9 (incl. ego); the original evidently keeps ego -> `eng_start_room` restores actors already assigned to the destination room.
* Walking uses a straight line; the original is the box-matrix walker (`b600`, `cbb6`, `bd1c`, `b842`, `b9b2`) with scaling (`d430`, `SA` block).
* Sound: AdLib music implemented (see "Sound" below); sound effects, PC speaker / CMS / Tandy drivers are not. Object-class/untouchable and `FUN_1000_4196` exact hit-testing approximated.


## Sound (AdLib) - recovered from MIDEMO.EXE segment 1fe8
* Sound modes (DS:0x1be6): 0 speaker, 1 ?, 2 CMS (Game Blaster, ports 0x220..), 3 **AdLib** (OPL2 at 0x388/0x389). Script var `soundcard` (48) = this mode; we set 3.
* `SO` block (from `0N` dir) holds sub-blocks `WA` (CMS), `AD` (AdLib), `SP` (speaker) - u32 size, 2-char tag. The demo's only sound is id 4 (the theme), started by script 201 in the dock room (`startSound`, op 0x1c; `startMusic` op 0x02 is the same call, `stopMusic` = stop all).
* Queue: `78f0` -> `58cc` pushes id, refcount `0x6218[id]++`; `isSoundRunning` (`77aa`) = refcount & 0x7f. Driver entry `223d` (AdLib).
* Timer ISR `1fe8:2470` hooked on INT 8: PIT reload 0x09d9 (2521) = 473.3 Hz; game timer +1 every 2nd call; BIOS chain every 26th. Music: 8-bit tempo accumulator (`+9`), on overflow one "step"; step runs the event stream. Format documented at the top of `engine/sound.c`. Percussion/rhythm instruments (`inst[13]`) and the effects sequencer (`28fc`) are decoded but not implemented/tested.
* AdLib init (`2100..`): reg 1=0, BD=0, 8=0, then 1=0x20 (waveform select). Note -> fnum: `n = note-0x1f; fnum = tab[n%12]; B0 = (n/12)<<2 | fnum>>8 | 0x20`, table DS:0x3d2. No velocity/volume handling.
* Our implementation: `engine/sound.c` (driver, ring buffer, game-clock-driven) + `engine/opl.c` (own OPL2 emulator, float, approximate envelope times, rhythm drums missing). Test: `SND_WAV=/tmp/t.wav ./mi_native ../mi_demo/ 2200 /tmp/x 0` writes mono 16-bit at 49716 Hz; `TRACE_SND=1`.

## Data loading / retail (session 3)
* Default: the shareware demo is embedded in `web/dist/index.html`. `?retail=true` (folder `retail/`, symlinked by `build.sh` to `web/retail`) or `?data=<folder>` fetches `000.LFL`, `DISK01..04.LEC` and `901..904.LFL` at runtime. Needs http (`cd web/dist && python3 -m http.server`), not `file://`. Docker compose mounts `./web/retail` read-only.
* Engine: `res_add_disk(n, ...)` + `res_set_index(...)`; each disk's `FO` block maps room -> LF offset on that disk (`g_lf_disk[room]`). Native: `./mi_native ../web/retail/ <frames> ...`.
* Retail observations: same block family (LE/FO/LF/RO/HD/BX/SA/BM/OI/OC/EX/EN/LS/SC/SO/CO); new `PA` (768-byte palette), `CC` (colour cycling), `SP`; `BM` uses 32-bit strip offsets (VGA codec); 84 rooms, ~202 scripts, ~172 sounds, ~181 costumes. It loads and boots to room 90 with the demo code; graphics/costumes/scripts untested.

## Retail VGA graphics (session 3)
* Room `PA` block: u16 0x0300 then (after 8-byte block header + 2) 256 x RGB 8-bit (first colours are the standard VGA/EGA set). `g_vga` = room has a `PA` block; compositor uses `g_pal_rgb`.
* Room `BM` / object `OI` images: `u32 total size, u32 offset per 8-px strip`; a strip = codec byte + first colour + LSB-first bit stream (codes 15-18 columns, 26-28 rows; code%10 = absolute-colour bits). Strips are the **HD height tall (can be 200, not only 144)**: decoding only 144 rows of a column-coded strip desynchronises the stream (vertical streak garbage). Our decoder is `gfx_decode_strip_vga`.
* Costumes: retail uses the same classic format byte 0x58 (16-entry palette, `0xd8` = mirrored) - the palette bytes are simply full VGA indices, so only the `& 15` in `pixel_color` had to go. Verified on the lookout pirate.
* Not yet for VGA: foreground mask planes (disabled when `g_vga`), actorOps sub 0x0d stores the raw index when `g_vga` (the demo EXE multiplies by 0x11 = EGA nibble pair; retail never used it in the intro, unverified against MONKEY.EXE - unpacked retail EXE is 112107 bytes vs demo 112075 but recompiled, not a patch), `CC` palette cycling, object `OI` layout check, actor palettes.

## Retail EXE vs demo EXE (session 3)
* `MONKEY.EXE` unpacks with the same LZEXE91 unpacker (112107 bytes vs demo 112075), recompiled (code layout differs, not a patch).
* Opcode table: retail DS image offset **0x1a4d2** (demo 0x1a5bc), 256 far pointers; the opcode -> handler partition is **identical** (103 handlers, same opcode groups), so opcode numbering/operand layout carries over. Handler code differs in size by a few instructions each (recompile).
* roomOps retail sub-ops 1..7 = same set as demo (1 camera limits, 2 colour remap with index < 0x100, 3 setScreen, 4 palette entry (not implemented), 5/6 shake on/off, 7 scale). Our `g_room_pal` is now 256 entries and applied to VGA output.
* Retail key variables after boot: mainmenu(50)=319 (F5), restart(42)=322 (F8), pause(43)=32 (space), cutscene exit(24)=27 (Esc). Menu/pause are EXE-native UI (not scripts): not implemented.
* Retail decompile route: `analyzeHeadless ... -import MIDEMO_UNP.EXE -processor "x86:LE:16:Real Mode" -postScript work/gh/scripts/MkFuncs.java addrs.txt out.c`, Ghidra linear = 0x10000 + image offset.

## Save / load menu (our own, session 3)
* The original F5 menu is EXE-native (plain box: Save/Load/Play/Quit buttons, DOS file I/O); we do not port it. Ours: `engine/menu.c` (overlay in the game's fonts, 10 slots, name entry, teal rounded buttons), opened with F5 while input is enabled; Esc / F5 / right click closes; the game is paused behind it, music continues.
* State: `engine/state.c` streams every module's state through one routine (`vm_state`, `eng_state`, `act_state`, `ui_state`, `snd_state`; `SIO()` writes or reads). Pointers into resource data (script code, object code, box matrix) are stored as (disk, offset). Zero-run packed (~6-10 KB). A save is refused if a pointer can't be described; header checks struct sizes and a per-game signature (demo/retail saves never mix). Music restarts from its beginning on load.
* Storage: browser `localStorage` keys `mi.<ns>.slotN.meta|data` (ns = `demo`, `retail` or the `?data=` folder), via `EM_JS` in `main_wasm.c`; native test harness uses `./saves/`. Test: `SAVE_AT=<frame> LOAD_AT=<frame> ./mi_native ...`.
* Not saved: message text on screen, palette-cycle state, exact music position.

## Sound effects (AdLib FX sequencer, session 3)
* Effect = `AD` block with `+8 != 0x80`: `+6` priority, `+7` slot 0..2 (slot s drives hardware voices 3s..3s+2), commands from `+8`: `01` instrument (15 B), `02` note (11 B = two 5-byte parameter notes), `80` loop channel, other byte = start of next channel, `FF` end. EXE `1fe8:223d` (start), `2963` (parse), `2a4c` (play), `2af6/2b4a/2d8b` (note stages/envelope); runs every 4th ISR (counter DS:0x172 = 4) = 118 Hz, **only while no music plays** (ISR `0x2470` calls `28fc` only if music pos [0] == 0; a new effect is ignored while music plays). Identical ISR code in retail.
* Tables verified byte-for-byte against the EXE (DS:0x440..0x4cc: bias, steps `1,4,6,...,1200`, adjust `0..65535 step 4369`, scale `255,7,63,15,63,15,63`, register base/mask/shift) and match ScummVM `player_ad.cpp`; LFSR seed 1 / xor 0xB8.
* Implementation: `engine/sound.c` (`fx_*`). Demo's door sound (id 2, script 26) renders as a ~0.2 s decaying high-pitched noise burst (feedback FM). Not verified by ear.

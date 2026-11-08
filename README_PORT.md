# Monkey Island demo -> browser (WASM)

Play: open `web/dist/index.html` (self-contained, works from `file://`). Rebuild: `web/build.sh` (uses the system `emcc`).
Native headless test: `cd engine && gcc -O1 -o mi_native main_native.c vm.c engine.c actor.c costume.c gfx.c res.c ui.c && ./mi_native ../mi_demo/ 1500 /tmp/f 100`
Browser test: `node web/test/cdp.js <url> (then eval `window.__speed=20`) <prefix> '<steps json>'`.

Layout: `RE_NOTES.md` (findings, addresses), `engine/` (portable C interpreter), `web/` (page + build), `work/` (unpacker, dumpers, Ghidra scripts, decompiles).

Web build note: the cursor is a CSS cursor (not drawn by the engine) so it follows the mouse at display rate; engine frames run at 60/VAR_TIMER_NEXT Hz (15 fps like the original).

Known gaps: no sound/music (stubbed); walking is straight-line (no walkbox pathfinding/scaling/masking);
room-change keeps actors already assigned to the destination room (unexplained deviation, see engine.c eng_start_room);
no save/load, F-key menus, or the end-of-demo contest screens were verified; most opcodes only exercised by the first minutes of the demo.

## Debug dialog
The `[debug]` link under the game opens a modal with a JSON snapshot of the engine (room, camera, mouse, actors, running scripts, inventory, room objects, non-zero variables, set flags, client info) plus `save_b64` - the packed save state (same bytes as a save slot, `state_save`), so a situation can be reproduced by loading it. Buttons: Refresh, Copy JSON, Download, Close (Esc / click outside also closes; game keys are ignored while it is open). C side: `mi_debug_json()` / `mi_debug_save()` in `engine/main_wasm.c`; JS in `web/index.template.html`. Hidden on touch layouts (the hint text is hidden there).

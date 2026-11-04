# BUGS.md — bug history and regression checklist

Each entry: **Symptom** → **Cause / wrong assumption** → **Fix** → **Regression check** (how to see it again).
Native test driver: `engine/mi_native <demo dir> <frames> <out-prefix> <dump-every>` with env `CLICKS=frame:x:y:btn,...` (btn 0 = mouse move only; send a move 2-4 frames
before a click so the hover script updates), `GOTO=<room>`, `ENTER=1` (enter the bar via its door), `DUMP=1`, `TRACE*` (see RE_NOTES.md). Browser: `web/test/cdp.js`.
Frame numbers below are approximate (dock scene starts ~frame 700).

## A. Data / format assumptions

| # | Symptom | Cause (wrong assumption) | Fix | Regression check |
|---|---|---|---|---|
| A1 | Index directories parsed as garbage (counts like 65436) | Assumed the whole `000.LFL` payload was XOR 0xFF; only the room *names* are | Directories are plain: `u16 n`, then `n × {u8 room, u32 off}` (5 bytes each) | `t_res`-style load: scripts 1..100 all resolve to an `SC` block |
| A2 | Text drawn as scrambled dots | Glyph bitmaps assumed row-padded to bytes | Glyph bits are one continuous stream (`bit = row*w + col`) | Boot: "Guybrush Threepwood" caption is legible |
| A3 | Costume decode read past the data / crashed | Offsets assumed relative to the data *after* the 6-byte block header | All costume offsets are relative to the block start (header included) | Room 3 (logo) → dock transition runs without ASAN errors; costume 2 pictures are 2x1, 6x5, 8x7… |
| A4 | Verbs parsed with shifted operands (huge variable numbers, "var oob") | `verbOps` sub-op 9 (*new*) was given a parameter byte; it has none | Sub-op 9 takes no argument | Boot script 31: verbs Open/Close/Push/Pull appear at the right places, no `VM error` lines |
| A5 | Verb panel ended at the wrong row / room drawn at y=0 | `roomOps 3` args read as (top, bottom) | They are (top, **height**): main view top=16, height=144 | Verb rows at y≈152..184, room image starts at screen row 16 |
| A6 | Verb with id 0 (the sentence line) never drew | Slot id 0 used as the "empty slot" marker | Separate `used` flag per verb slot | Hover a door: "Walk to door"/"Open door" appears centred under the room |

## B. Opcode / script semantics

| # | Symptom | Cause | Fix | Regression check |
|---|---|---|---|---|
| B1 | Cutscene caller (script 1) never resumed; game stuck after intro | `freezeScripts` froze the script that started the cutscene | Remember the cutscene slot (`DS:57ae` in the EXE) and unfreeze it after freezing | After the headshot intro the Lucasfilm logo and then the dock appear |
| B2 | Boot reached the dock but Guybrush was missing | Logo room's exit script (`EX`) parks actors 1..9 in room 0, killing ego | **Unresolved deviation**: actors already assigned to the destination room are restored (room *and* x/y) after the exit scripts run | Ego appears at (360,132) off the right edge and walks in; actor 1 `room 55` after the title |
| B3 | Troll ("none shall pass") fired immediately; Guybrush walked in from the top-left | Restoring only the actor's room: `putActorInRoom(a,0)` also zeroes x,y so ego was at x=0 and the troll script (reads ego x) triggered | Restore position as well | No troll while Guybrush is on the right; it appears only when he walks left |
| B4 | Title cutscene showed the verb panel | `saveRestoreVerbs` marked verbs saved but they were still drawn / hit-tested | Skip saved verbs in draw and click tests | During the title ("The Secret of Monkey Island") no verbs are visible; they appear afterwards |
| B5 | Clicking an object walked to the click point instead | `findObject` iterated last→first; the original returns the **first** match in room order | First match in room order | Open verb + door: `walkTo(1,67,122)` (door walk spot), not the click |
| B6 | Hover/click used wrong object/mouse y | `VAR_VIRT_MOUSE_Y` was screen y; should be `y - view_top` (−1 outside the view), x = `mouse x + camera left` | Set both per frame | Hover over the bar door shows "Walk to door"/"Open door" |
| B7 | Verb names lacked "Look at door" etc. | `0xFF 5/6/4/7 <var>` escapes substituted the *variable number*; they refer to the variable's **value**; names are re-expanded every draw | Resolve through `g_vars[]` at draw time | Sentence line text follows hover/verb choice |
| B8 | Walking through a door left the actor invisible and the scene unchanged | Door scripts end with `putActorInRoom` + `putActor` + `actorFollowCamera`; `FUN_1000_7e3c` makes the **follow** switch to the actor's room. Follow was only moving the camera | `eng_follow_actor`: if the actor's room ≠ current, start that room | `ENTER=1`, click *Open* then door, then *Walk to* door → `start_room 55`, ego at (361,132) |
| B9 | Cutscene end / `actorFollowCamera` made the camera jump | Follow re-centred unconditionally | Only re-centre when the actor is outside strips 10..30 of the view (`FUN_1000_151e`) | After the title the dock stays at camera 160 while Guybrush walks in; no jump |
| B10 | ESC left the speech bubble up for a moment | Override restore did not clear the message | Clear message state when the cutscene is aborted | Press ESC during the headshot intro: text disappears immediately |

## C. Actors, costumes and animation

| # | Symptom | Cause | Fix | Regression check |
|---|---|---|---|---|
| C1 | Walking sprite had a front-facing head painted over it ("double head") | Limb with animation command `0x79` (*stop*) was still drawn; walk anims stop the head limb because the head is baked into limb 0 | Do not draw stopped limbs | Walk Guybrush left/right: one head only |
| C2 | Animations stalled/ended early | Commands `0x78`/`0x7C` (counters) treated as end-of-animation | Skip them while stepping | Walk cycle loops continuously |
| C3 | Guybrush kept facing the screen / wrong way after a turn, head showed front while body showed back | Only limbs covered by the new animation were re-decoded; the head limb kept its old direction | After starting an anim, re-decode other active limbs with `(frame & ~3) | newdir` (ScummVM `setDirection`) | Walk to the dock door (obj 422, dir 3): back of head and back view |
| C4 | "Moonwalking" / troll facing the wrong way | A decompile-based "fix" inverted the mirror rule (the flag at `DS:6f0c` means *not mirrored*) | `mirror = (facing == 0) ^ (format & 0x80)` (verified visually; pictures face the other way than the decompile suggests) | Walk right: faces/moves right; troll faces east toward the dock |
| C5 | Walk-to-object arrived with the wrong stance (default right) | Arrival direction ignored | `walkActorToObject` takes the object's `actordir` (OC byte 17, bits 0-2); plain `walkActorTo` keeps travel direction | Dock door: ends facing north (3) with back to camera |
| C6 | Entering the bar: faced away from camera | Used a geometric "face the object" rule | `loadRoomWithEgo` faces `objdir ^ 1` (handler 726e) | Enter the bar through its left door: Guybrush faces right |
| C7 | After the title Guybrush faced right, DOSBox faces the screen | `animateActor(0xF8+dir)` only stored the direction and did not restart the stand animation | Restart stand anim for the new facing | End of dock cutscene: front-on stand pose |
| C8 | Ego walked diagonally "down" when entering | `putActor(…,360,100)` lands outside all walkboxes; the original snaps actors into the nearest box (`adjustXYToBeInBox`) | Snap on put, and snap walk destinations | Enters at (360,132) and walks flat to (235,132) |
| C9 | Troll spawned too far right (x=147 instead of 100) | Snapping applied to actors that should **ignore boxes** | Class 22 (`IgnoreBoxes`) actors skip snapping; also class 20 NeverClip / 21 AlwaysClip for masks | Troll `putActor(11,100,132)` stays at x=100 (inside its object rect 80..128) |
| C10 | Multi-limb costumes drifted when mirrored | Mirrored x-move accumulated with the wrong sign | Always accumulate `+movex`, apply the mirror when placing | Troll body parts line up |
| C11 | Clicking the ceiling in the bar walked Guybrush up the wall / behind the right pirates incorrectly | Walk destination not constrained to the walkable area | Snap destination into the nearest box; keep steps inside boxes | `GOTO=52`, click near the ceiling: ego stops at the box edge (y≈128) |
| C12 | Actors walked over pirates/tables as if they were background (z-order) | Object **foreground masks** ignored | Parse mask stored after each `OI` image (u16 size, strip offsets, RLE rows of 8 pixels), hide actor pixels where set (boxes with `mask≠0`) | Bar: Guybrush at (200,131) is hidden behind the table/pirate below the chest |

## D. Platform / rendering / performance

| # | Symptom | Cause | Fix | Regression check |
|---|---|---|---|---|
| D1 | Cursor felt laggy | Cursor drawn into the 15 fps game frame | Web build uses a CSS crosshair (`-DJS_CURSOR`); engine only reports visibility | Cursor tracks the mouse at display rate |
| D2 | Walkbox snapping broke only in the WASM build | `long` is 32-bit in WASM; `1L << 40` and squared distances overflowed | Use `long long` | Browser: dock scene and walking behave like native (`web/test/cdp.js`, `?speed=20`) |
| D3 | Copyright text overlapped verbs | Added the view offset to an absolute screen y | Explicit print positions are screen coordinates; only actor-relative text adds the view top | Title cutscene: copyright lines below the room image |
| D4 | Persisting stale text in the sentence line ("WaWalk to") | Verb redraw did not erase the previous (longer) text | Remember last drawn rect and erase it before redrawing | Hover different objects: no residue |

| B11 | Talking to the troll: every dialog choice made Guybrush say "Open" | Dialog choices are **verbs** reusing the ids of the normal verbs, which the script first *saves* (`saveRestoreVerbs`). Lookup by id returned the saved original (Open, id 1) instead of the new choice, so clicks resolved to the old verb | Lookups ignore saved verbs; *restore* kills the live verb with the same id before reviving the saved one; *delete-saved* implemented (handler 8b90) | Walk left (troll appears), Talk to troll, pick option 2 → "Tough noogies!", then the normal verb panel returns |

## E. Known open items (not bugs fixed yet)

* Exit-script actor kill (B2) is worked around, not explained.
* Walking is straight-line + snapping, not the original box-matrix path walker (`FUN_1000_b600` family); scaling slots (`SA` block) unused.
* No sound/music; AdLib/PC-speaker resources (`0N`) are ignored.
* The DOSBox-X oracle (`work/oracle/run.sh`) could not be used past the intro (crash/black frames), so dock/bar behaviour was checked against the script logic and user reports.

## F. Suggested automated regression set

1. `ENTER=1` + scripted clicks above (B5/B8/C6): assert `room`, ego position and `facing` from `DUMP=1`.
2. Frame hash/diff of the headshot intro frame (A2/B10) against the stored DOSBox-X comparison (99.2% match baseline).
3. `mi_asan` build (`-fsanitize=address,undefined`) over 3000 frames of the intro + dock (A3).
4. `web/test/cdp.js` smoke test: page loads from `file://`, `window.__frames` increases, room reaches 55.

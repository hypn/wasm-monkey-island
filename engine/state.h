#ifndef STATE_H
#define STATE_H
#include "scumm.h"
/* Save-game serialisation: every module streams its state through one routine that both writes and reads
 * (io->saving).  Pointers into the resource data (script code, object code, box matrix ...) are stored as
 * (disk, offset) references so a save is independent of where the data was loaded. */
/* Save format versioning: see RE_NOTES.md "Save-game versioning".  io->ver is the version of the data being read
 * (== STATE_VERSION when saving); a module whose layout changed branches on it and migrates old data. */
#define STATE_VERSION 4
typedef struct { u8 *buf; u32 pos, cap; int saving, err, ver; } StateIO;
void sio_bytes(StateIO *io, void *p, u32 n);
#define SIO(io, x) sio_bytes((io), &(x), (u32)sizeof(x))
void sio_ptr(StateIO *io, const u8 **p);
int  state_save(u8 **out, u32 *len);          /* caller frees *out */
int  state_load(const u8 *data, u32 len);     /* 0 = ok */
#endif

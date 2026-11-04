# syncstream in_avail

`basic_syncbuf::in_avail` returns how many stored bytes remain at the
offset: `_M_off < _M_size ? _M_size - _M_off : 0`. It does not write, does
not move the offset, and does not load through the pointer. A null buffer
whose size was forced to 0 returns 0. An offset at or past the size
returns 0 and does not subtract.

This is not PROVED and not CLEAN.

# spanstream data and in_avail

`basic_spanbuf::data` returns the stored `char*`. `size` returns the stored
size. `in_avail` returns how many bytes remain at the offset:
`_M_off < _M_size ? _M_size - _M_off : 0`. None of them writes, and none
of them loads through the pointer. A null buffer of size 0 returns
`nullptr` from `data`, `0` from `size`, and `0` from `in_avail`. An offset
at or past the size returns 0 from `in_avail` and does not subtract.

This is not PROVED and not CLEAN.

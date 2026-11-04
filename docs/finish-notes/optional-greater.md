# optional greater, swap, and emplace

`operator>`, `swap`, and `emplace` live on the existing `<optional>` model.
`operator==` and `operator<` are unchanged.

Nothing here is `PROVED`. Nothing here is `CLEAN`.

`operator>` of two disengaged optionals is false and does not read `T`.
A disengaged left is not greater. An engaged left and a disengaged right
is greater. Two engaged optionals compare the stored values with
`operator>`.

`emplace` of one argument constructs `T` in the existing storage. A
disengaged optional is not destroyed first. An engaged optional destroys
the stored `T`, then constructs the new one. The returned reference is
that stored `T`.

`swap` of two disengaged optionals does not name `T`. One engaged and one
disengaged move-constructs into the empty storage and ends the source
lifetime. Two engaged optionals exchange the values by move. Self-swap
returns and does not destroy the stored `T`.

`operator<=`, `operator>=`, `operator<=>`, `hash`, `make_optional`, and
comparison with a bare `T` stay undeclared.

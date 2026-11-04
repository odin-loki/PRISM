# string_view contains, find_of, and at

`basic_string_view::contains`, `find_first_of`, `find_last_of`,
`find_first_not_of`, `find_last_not_of`, and `at` live on the existing
`<string_view>` model. `find`, `rfind`, `starts_with`, `ends_with`,
`compare`, and `substr` are unchanged.

Nothing here is `PROVED`. Nothing here is `CLEAN`.

`contains` of an empty needle is true and does not read. `contains` of a
character on an empty view is false and does not read a null pointer.
`find_first_of` and `find_last_of` of an empty needle are `npos` and do
not read. `find_first_not_of` of an empty needle returns `pos` when
`pos < size()`. `find_last_not_of` of an empty needle returns the last
index at or before `pos`. A miss is `npos`, not a contract failure.

`at` of an index outside the view is `__prism_check` with class
`MEM-OOB-READ` and message
`std::basic_string_view::at index is outside the view`, then
`__builtin_trap`, before the character is read. `at(0)` on an empty view
takes that path.

The `CharT*` overloads of the find and contains members stay undeclared.
`copy` and the rest of `<string_view>` stay omitted.

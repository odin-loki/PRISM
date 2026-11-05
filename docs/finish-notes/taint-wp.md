# taint and wp

Slice on `e59a8b3df`. Owned files: `src/prism/stages/taint.cpp`, `src/prism/stages/wp.cpp`, their doctests, this note. Contract-keyword case sensitivity stays in `common.cpp`.

## taint: `*` is stripped only for scanf

`take()` used to strip every leading `&` and `*` from every source. Nothing in the doctests locked that. It mis-labels a non-scanf buffer:

- `fread(*p)` / `read(fd, *p, n)` / `fgets(*line)` writes through the pointer expression. Stripping `*` records the different name `p` or `line`, so `system(p)` is reported.
- `fread(* buf)` does the same after the following space is trimmed: the expression collapses to the identifier `buf`.

Now only scanf strips a leading `*` as well as `&`. Every other source strips a leading `&` only.

What is kept:

- Leading `&` still comes off every source, and a `[...]` subscript is still cut, then the ends are stripped. `&buf`, `& buf`, and `&buf[i]` still name `buf`. The final strip is deliberate: a space left in `& buf` would not match an identifier.
- scanf still strips a leading run of `&` and `*`, including a space after the star (`scanf("%s", * p)` names `p`, and `*&buf` names `buf`).

What stays tainted:

- A non-scanf label that is not an identifier (`*p`, `* buf`, `*&buf`, `&*buf` after the `&` is removed) is compared to the sink argument with whitespace ignored. An identifier character on either side does not count (`*p` is not `*ptr` or `x * p`).
- So `fread(*p); system(*p);` and `fread(*p); system(*p + 1);` are still TAINT-SINK, and `fread(*p); system(p);` is not.
- `&*buf` and `*&buf` are not cancelled down to `buf` on those sources. `fread(&*buf)` taints `*buf`, not `buf`. scanf still cancels both.

`getenv` assignment, `fread(buf)`, and `strcat` of a getenv result are unchanged.

## wp: empty returns were already kept

The skip of an empty stripped return (`if (g.empty()) continue`) was removed in `eff6be059`. The return regex was widened in `a8ed6e498` to `\breturn\b\s*([^;]*);`, so `return;`, `return(x);`, and `return ();` all match.

- The old `continue` dropped an empty return path, so a later `return x + 1` could QED the ensures.
- The stage now keeps both `return;` and `return   ;` as `""`. The substituted VC is `()` around that empty expression, which is not a tautology. `check_with_assume` then refuses the bare return as ERROR. `tests/cpp/test_wp_harness.cpp` already locks this.
- `return ()`, `return ( );`, and `return(( ));` are not empty after `strip`. They stay as `()`, `( )`, and `(( ))`. Peeling the parentheses and then skipping the empty text would drop the same path. A new doctest locks the stored expression, that it is not `wp_qed`, and that the VC text still contains the substituted parentheses.
- Truly empty text is still skipped: an empty `&&` conjunct, an empty predicate (`encode` returns nullopt), and a function with no `return` (the VC is the ensures alone, because there is no return path to drop).

No wp behavior change in this slice.

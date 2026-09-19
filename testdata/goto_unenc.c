int with_goto(int x) {
    goto skip;
    x = x + 1;
skip:
    return x;
}

struct s { int x; };

int null_branch(struct s *p) {
    if (p == 0) {
        return p->x;
    }
    return p->x;
}

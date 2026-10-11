#pragma once
#include <stdexcept>
template <class Read, class Stage>
static void transferReedLinks(unsigned n, unsigned sh, unsigned dh, unsigned sa, unsigned da,
                              Read ptr, Stage stage) {
    unsigned prev = ptr(n + 4), next = ptr(n), tail = ptr(dh + 4), sc = ptr(sa), dc = ptr(da);
    if (!sh || !dh || sh == dh || !prev || !next || !tail || sc == 0 || dc >= 4096 ||
        ptr(prev) != n || ptr(next + 4) != n || ptr(tail) != dh || ptr(ptr(dh) + 4) != dh)
        throw std::runtime_error("Ownership links invalid");
    stage(prev, next);
    stage(next + 4, prev);
    stage(n, dh);
    stage(n + 4, tail);
    stage(tail, n);
    stage(dh + 4, n);
    stage(sa, sc - 1);
    stage(da, dc + 1);
}

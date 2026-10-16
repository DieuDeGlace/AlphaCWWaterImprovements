#pragma once
#include <stdexcept>

// Moves one node from a source chunk's circular, doubly linked vegetation list
// to the destination chunk's list (appended before the destination head).
//
// Each node has `next` at +0 and `prev` at +4; list heads are sentinel nodes.
// `sourceCount`/`destinationCount` are the addresses of the lists' item counts.
// `ptr(address)` reads a pointer and `stage(address, value)` records a write;
// nothing is written directly, so the caller can journal and apply the edits.
// Throws std::runtime_error if any link is inconsistent.
template <class Read, class Stage>
static void transferReedLinks(unsigned node, unsigned sourceHead, unsigned destinationHead,
                              unsigned sourceCount, unsigned destinationCount, Read ptr,
                              Stage stage) {
    unsigned prev = ptr(node + 4), next = ptr(node), tail = ptr(destinationHead + 4);
    unsigned sourceItems = ptr(sourceCount), destinationItems = ptr(destinationCount);
    if (!sourceHead || !destinationHead || sourceHead == destinationHead || !prev || !next ||
        !tail || sourceItems == 0 || destinationItems >= 4096 || ptr(prev) != node ||
        ptr(next + 4) != node || ptr(tail) != destinationHead ||
        ptr(ptr(destinationHead) + 4) != destinationHead)
        throw std::runtime_error("Ownership links invalid");
    // Unlink from the source list.
    stage(prev, next);
    stage(next + 4, prev);
    // Link in after the destination's current tail.
    stage(node, destinationHead);
    stage(node + 4, tail);
    stage(tail, node);
    stage(destinationHead + 4, node);
    stage(sourceCount, sourceItems - 1);
    stage(destinationCount, destinationItems + 1);
}

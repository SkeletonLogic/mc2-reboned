#pragma once
// PORT: Stack capture is compiled out in the browser release configuration;
// Heap.cpp only needs complete placeholder records for its dormant helpers.
struct CONTEXT { unsigned long reserved[16]; };
struct STACKFRAME { unsigned long reserved[16]; };

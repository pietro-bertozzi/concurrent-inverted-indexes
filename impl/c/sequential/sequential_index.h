// The C sequential index: one TermMap (our own hash map, see _common/hashmap.h), no
// synchronisation. Documents are added in increasing ID order, so every posting list is already
// sorted and finish() has nothing to do.
#pragma once
#include "index.h"

// Creates an empty sequential index.
Index *sequential_index_new(void);

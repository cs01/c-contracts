/* A translation unit whose only job is to put ZSTD_wildcopy's contract in
   front of the tool: it is declared in a header, and `check` and `prove` both
   read the contract out of an ordinary parse. */
#include "zstd_internal.h"

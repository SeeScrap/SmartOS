// The ESP32-S3 side of the port seam. See port/kport.h for what it is for.
//
// R4a creates it with nothing in it but this note: the attribute macros are
// header-only, so the first thing that needs a translation unit arrives in
// R4b. Having the file in the build from the start means each later stage is
// a change to one file rather than a change to the build as well.

#include "port/kport.h"

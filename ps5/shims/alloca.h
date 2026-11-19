#pragma once
/* PS5 Payload SDK ships no <alloca.h>; clang provides __builtin_alloca. */
#include <stddef.h>
#define alloca(size) __builtin_alloca(size)

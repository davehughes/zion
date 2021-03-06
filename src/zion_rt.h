#pragma once

/* for now let's go big and not worry about it */
typedef double zion_float_t;

#include <unistd.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <assert.h>
#include <signal.h>
#include <locale.h>
#include <langinfo.h>
#include "colors.h"
#include "type_kind.h"

typedef int64_t zion_int_t;
typedef int64_t zion_bool_t;
typedef int32_t type_id_t;
typedef int32_t type_kind_t;

#define int do_not_use_int
#define float do_not_use_float
#define double do_not_use_double

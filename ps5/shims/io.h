/* MSVC <io.h> stand-in: the low-level file helpers the port uses. */
#ifndef OOGIE_PS5_IO_H
#define OOGIE_PS5_IO_H
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef _access
#define _access access
#endif
#ifndef _read
#define _read read
#endif
#ifndef _write
#define _write write
#endif
#ifndef _close
#define _close close
#endif
#ifndef _open
#define _open open
#endif
#ifndef _commit
#define _commit fsync
#endif
#endif

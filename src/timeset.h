#ifndef TIMESET_H
#define TIMESET_H

#include <stdbool.h>
#include <stdint.h>

/* set the system clock from a unix-millisecond target.
   Returns true on success. When an explicit privilege request is needed
   (running as non-SYSTEM), tries to enable SeSystemtimePrivilege first. */
bool timeset_system_time(int64_t unix_ms);

/* enable SeSystemtimePrivilege in the current token; true if granted. */
bool timeset_enable_priv(void);

/* true when the process runs with an elevated (admin) token. */
bool timeset_is_elevated(void);

#endif /* TIMESET_H */

#ifndef SOLAR_STATUS_H_
#define SOLAR_STATUS_H_

#include <stddef.h>

/* Sample the nPM1300 on a dedicated thread so the 30 s harvest window
 * keeps running between gateway syncs.
 */
void solar_status_start(void);

/* Write the latest snapshot as one JSON object. Returns the length
 * written, excluding the terminating NUL, or a negative errno.
 */
int solar_status_format(char *buf, size_t len);

#endif /* SOLAR_STATUS_H_ */

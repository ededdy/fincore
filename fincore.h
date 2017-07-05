#ifndef _LINUX_FINCORE_H
#define _LINUX_FINCORE_H

#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

enum {
	FPG_uptodate = 0,
	FPG_active,
	FPG_referenced,
	FPG_dirty,
	FPG_writeback,
	FPG_unevictable,
	FPG_reclaim,
	FPG_private,
	FINCORE_MAX_ARGUMENT_SIZE = 120	/* Max size of fincore arguments
					 * converted to string.  This should 
					 * be enough to pass four u64
					 * seperated by space and argument
					 * as string terminated by '\0". */
};

extern int fincore_helper(int fd, loff_t start, size_t length,
		unsigned char **out_vec);
extern void print_results(unsigned char *vec, size_t length);

static inline int fincore(int fd, size_t *out_length, unsigned char **out_vec) {
	struct stat buf;

	if (fcntl(fd, F_GETFD) == -1)
		return errno;
	if (fstat(fd, &buf) == -1)
                return errno;
        *out_length = buf.st_blocks * (1 << 9);
	return fincore_helper(fd, 0, *out_length, out_vec);	
}
#endif /* _LINUX_FINCORE_H */


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <limits.h>

#include "fincore.h"

static const char *header[] =
{
"[State]*  - R:referenced A:active U:uptodate D:dirty W:writeback B:buffers ",
"	     U:unevicatable R:reclaim                                       ",
"                                                                           ",
"               Index                  Run            Status                ",
NULL
};

struct { 
	unsigned char mask; 
	const char *name; 
} page_flags[] = { 
	{1 << FPG_uptodate,	"U:uptodate"}, 
	{1 << FPG_active,	"A:active"},
	{1 << FPG_referenced,	"R:referenced"},
	{1 << FPG_dirty,	"D:dirty"}, 
	{1 << FPG_writeback,	"W:writeback"}, 
	{1 << FPG_unevictable,	"U:unevictable"}, 
	{1 << FPG_reclaim,	"R:reclaim"}, 
	{1 << FPG_private,	"B:buffers"}, 
}; 

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

void print_results(unsigned char *vec, size_t length)
{
	int i;
	size_t pindex, run, start;
	char str_flags[9] = {0};

	if (!length)
		return;
	i = 0;
	while(header[i])
		fprintf (stdout, "%s\n", header[i++]);
	pindex = 0;
	do {
		unsigned char flags;

		run = 0;
		while (!vec[pindex]) {
			pindex++;
			if (pindex == length - 1)
				return;
		}
		start = pindex;
		flags = vec[pindex];
		while (vec[pindex] && vec[pindex] == flags) {
			run++; pindex++;
			if (i == length - 1)
				break;
		}
		for (i = 0; i < ARRAY_SIZE(page_flags); i++) 
			str_flags[i] = flags & page_flags[i].mask ?
				page_flags[i].name[0] : '_';
		fprintf(stdout, "%20zu %20zu %18s\n", start, run, str_flags);
	} while (pindex < length);
}

int fincore_helper(int fd, loff_t start, size_t length, unsigned char **out_vec)
{
	int fd_fincore;
	unsigned char *address;
	size_t to_write, written, nr_pages;
	size_t page_size = getpagesize();
	char args[FINCORE_MAX_ARGUMENT_SIZE];

	errno = 0;
	if (fcntl(fd, F_GETFD) == -1)
		return errno;
	fd_fincore = open(FINCORE_DIR_PATH"-dir/params",  O_WRONLY);
	if (fd_fincore == -1)
		return errno;
	if (!length)
		return EINVAL;
	/* Pass one byte for each page. */
	nr_pages = (length + page_size - 1) / page_size;
	address = mmap(NULL, nr_pages * page_size, PROT_READ|PROT_WRITE,
			MAP_SHARED|MAP_ANON, 0, 0);
	if (address == MAP_FAILED)
		goto err_out;
	snprintf(args, FINCORE_MAX_ARGUMENT_SIZE, "%d %zu %zu %p\n", fd, start,
		length, address);
	to_write = strlen(args) + 1;
	written = write(fd_fincore, args, to_write);
	if (written == -1)
		return errno;
	*out_vec = address;
	return 0;
err_out:
	close(fd_fincore);
	return errno;
}

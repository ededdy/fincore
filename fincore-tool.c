#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "fincore.h"

void usage(const char * progname) {
	fprintf (stderr, "Usage: %s [path-to-file] [optional-length]\n"
			"\n", progname);
}

int main ( int argc, char **argv )
{

	int fd, err;
	unsigned char *vec;
	size_t length;
	char *progname = (progname = strrchr (argv[0], '/')) ?
		progname++ : (progname = argv[0]);

	if (argc < 2) {
		usage(progname);
		return EXIT_FAILURE;
	}
	fd = open(argv[1],  O_RDONLY);
	if (fd == -1) {
		perror("open");
		return errno;
	}
	err = fincore(fd, &length, &vec);
	if (err) {
		perror("fincore");
		return EXIT_FAILURE;
	}
	print_results(vec, length);
	close(fd);
	return EXIT_SUCCESS;
}

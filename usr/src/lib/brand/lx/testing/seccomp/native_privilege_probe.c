/*
 * CDDL version 1.0, http://www.illumos.org/license/CDDL.
 * Native illumos helper. No file changes or external commands.
 */
#include <priv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	priv_set_t *set;
	int expected, permitted, effective;

	if (argc != 3)
		return (2);
	expected = atoi(argv[2]);
	if ((set = priv_allocset()) == NULL)
		return (1);
	if (strcmp(argv[1], "prepare") == 0) {
		priv_emptyset(set);
		if (priv_addset(set, PRIV_PROC_SETID) != 0 ||
		    setpflags(PRIV_AWARE, 1) != 0 ||
		    setppriv(PRIV_ON, PRIV_INHERITABLE, set) != 0 ||
		    setppriv(PRIV_OFF, PRIV_EFFECTIVE, set) != 0 ||
		    setppriv(PRIV_OFF, PRIV_PERMITTED, set) != 0) {
			perror("prepare native privilege sets");
			return (1);
		}
		if (getppriv(PRIV_PERMITTED, set) != 0 ||
		    priv_ismember(set, PRIV_PROC_SETID))
			return (1);
		execl(argv[0], argv[0], "check", argv[2], (char *)NULL);
		perror("native privilege exec");
		return (1);
	}
	if (strcmp(argv[1], "check") != 0)
		return (2);
	if (getppriv(PRIV_PERMITTED, set) != 0)
		return (1);
	permitted = priv_ismember(set, PRIV_PROC_SETID);
	if (getppriv(PRIV_EFFECTIVE, set) != 0)
		return (1);
	effective = priv_ismember(set, PRIV_PROC_SETID);
	priv_freeset(set);
	if (permitted != expected || effective != expected) {
		fprintf(stderr, "proc_setid after exec: P=%d E=%d, "
		    "expected %d\n", permitted, effective, expected);
		return (1);
	}
	return (0);
}

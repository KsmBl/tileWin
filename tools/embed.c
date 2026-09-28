/*
 * Turns a file into a C header holding its bytes as a string, for a program
 * to show a text of the source tree it was built from (the Matrix screen saver
 * raining its own code).
 *
 *   tilewin-embed <input> <output.h> <symbol>
 *
 * writes "static const char <symbol>[] = { ..., 0 };".
 */
#include <stdio.h>

int main(int argc, char **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: %s <input> <output.h> <symbol>\n", argv[0]);
		return 2;
	}
	FILE *in = fopen(argv[1], "rb");
	if (!in) {
		perror(argv[1]);
		return 1;
	}
	FILE *out = fopen(argv[2], "w");
	if (!out) {
		perror(argv[2]);
		fclose(in);
		return 1;
	}
	fprintf(out, "/* Made from %s by tilewin-embed. */\nstatic const char %s[] = {", argv[1],
		argv[3]);
	int c, n = 0;
	while ((c = fgetc(in)) != EOF) {
		fprintf(out, "%s0x%02x,", n % 16 ? " " : "\n\t", (unsigned)c);
		n++;
	}
	fprintf(out, "\n\t0x00\n};\n");
	fclose(in);
	return fclose(out) == 0 ? 0 : 1;
}

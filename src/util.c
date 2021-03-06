#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

void print_hex(unsigned char *s, size_t n) {
	unsigned int i;
	for (i=0; i<n; ++i)
		printf("%02x", (unsigned int)s[i]);
	printf("\n");
}

void hextobin(unsigned char *v, unsigned char *s, size_t n) {
	unsigned int i;
	char _t[3];
	unsigned char *p = s;
	for (i=0; i<n; ++i) {
		memcpy(_t, p, 2);
		_t[2] = '\0';
		v[i] = (int)strtol(_t, NULL, 16);
		p += 2;
	}
}

char *c_strdup(const char *str) {
	int n = strlen(str) + 1;
	char *dup = malloc(n);
	if(dup)
		strcpy(dup, str);
	return dup;
}

int isnull(unsigned char *s, size_t n) {
	unsigned int i, j = 0;
	for (i=0; i<n; ++i)
		if (!s[i])
			j++;
	return (j == n) ? 1 : 0;
}
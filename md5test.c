
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include "md5.h"

int main(int argc, char *argv[])
{
	struct md5_state ctx;
	char buf[8192];
	int n;
	uint8_t out[MD5_DIGEST_SIZE];

	md5_init(&ctx);

	while ((n = read(0, buf, sizeof(buf))) > 0)
		md5_update(&ctx, buf, n);
	md5_final(&ctx, out);

	for (n = 0; n < MD5_DIGEST_SIZE; n++)
		printf("%02x", out[n]);
	printf("\n");
	exit(0);
}

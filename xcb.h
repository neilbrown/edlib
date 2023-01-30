
static inline xcb_connection_t *xcb_connect_auth(const char *display,
						 const char *auth,
						 int *screen)
{
	extern char **environ;
	char **environ_orig = environ;
	char *env[2];
	char xa[1024];
	xcb_connection_t *ret;

	if (!auth || strlen(auth) > 1000)
		return xcb_connect(display, screen);

	strcpy(xa, "XAUTHORITY=");
	strcat(xa, auth);
	env[0] = xa;
	env[1] = NULL;
	environ=env;
	ret = xcb_connect(display, screen);
	environ = environ_orig;

	return ret;
}

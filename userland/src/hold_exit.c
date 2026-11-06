/*
 * datamigrator is asked to migrate, does its work and exits at once.  The
 * client in DataMigration is still waiting for the XPC connection to become
 * active at that moment and never sees the event: keeping the daemon alive
 * gives launchd's activation and the reply the time to happen.
 */
#include <mach-o/dyld.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dlfcn.h>

static int out = -1;

static void
say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (out >= 0 && n > 0) {
		write(out, buf, (size_t) n);
	}
}

static void
hold_exit(int code)
{
	Dl_info info;

	say("[hold] %s[%d] exit(%d) held: the process stays up\n", getprogname(), getpid(), code);
	if (dladdr((void *) __builtin_return_address(0), &info) && info.dli_fname != NULL) {
		const char *slash = strrchr(info.dli_fname, '/');

		say("[hold]   from %s+%#lx\n", slash ? slash + 1 : info.dli_fname,
		    (unsigned long)((uintptr_t) __builtin_return_address(0) - (uintptr_t) info.dli_fbase));
	}
	for (;;) {
		sleep(60);
	}
}

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void *replacement; const void *replacee; } \
	_interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = \
	{ (const void *)(uintptr_t) &_replacement, (const void *)(uintptr_t) &_replacee }

DYLD_INTERPOSE(hold_exit, exit);

__attribute__((constructor))
static void
hold_init(void)
{
	const char *name = getprogname();

	out = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (out < 0) {
		out = 2;
	}
	if (name == NULL || strstr(name, "datamigrator") == NULL) {
		say("[hold] loaded in %s[%d] (not held)\n", name ? name : "?", getpid());
		return;
	}
	say("[hold] holding exit in %s[%d]\n", name, getpid());
}

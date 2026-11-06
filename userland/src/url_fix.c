/*
 * A nil path handed to -[NSURL initFileURLWithPath:] ends a process with
 * NSInvalidArgumentException.  Several daemons on this port do that during
 * launch (iconservicesagent among them), and the clients waiting for their
 * XPC replies then wait forever.  Give Foundation a harmless path instead.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

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
where(const char *tag, uintptr_t addr)
{
	Dl_info info;
	const char *name = "?";
	unsigned long off = 0;

	if (addr != 0 && dladdr((void *) addr, &info) && info.dli_fname != NULL) {
		const char *slash = strrchr(info.dli_fname, '/');
		name = slash ? slash + 1 : info.dli_fname;
		off = (unsigned long)(addr - (uintptr_t) info.dli_fbase);
	}
	say("[url]   %s %#lx %s+%#lx\n", tag, (unsigned long) addr, name, off);
}

static id (*orig_init_path)(id, SEL, id);
static id (*orig_init_path_dir)(id, SEL, id, BOOL);
static id (*orig_file_path)(id, SEL, id);

static id
hook_init_path(id self, SEL selector, id path)
{
	if (path == NULL) {
		say("[url] initFileURLWithPath: nil\n");
		where("from", (uintptr_t) __builtin_return_address(0));
		path = (id) CFSTR("/");
	}
	return orig_init_path(self, selector, path);
}

static id
hook_init_path_dir(id self, SEL selector, id path, BOOL directory)
{
	if (path == NULL) {
		say("[url] initFileURLWithPath:isDirectory: nil\n");
		where("from", (uintptr_t) __builtin_return_address(0));
		path = (id) CFSTR("/");
	}
	return orig_init_path_dir(self, selector, path, directory);
}

static id
hook_file_path(id self, SEL selector, id path)
{
	if (path == NULL) {
		say("[url] fileURLWithPath: nil\n");
		where("from", (uintptr_t) __builtin_return_address(0));
		path = (id) CFSTR("/");
	}
	return orig_file_path(self, selector, path);
}

/*
 * The same nil URL, one call later: iconservicesagent asks NSFileManager to
 * enumerate a directory whose URL is nil and dies of NSInvalidArgumentException
 * ("enumeratorAtURL:... URL is nil"), which leaves lsd's IconServices XPC
 * waiting for ever.  Say an empty directory instead.
 */
static id (*orig_enumerator)(id, SEL, id, id, unsigned long, id);

static id
hook_enumerator(id self, SEL selector, id url, id keys, unsigned long options, id handler)
{
	if (url == NULL) {
		id empty = ((id (*)(id, SEL)) objc_msgSend)((id) objc_getClass("NSArray"),
		    sel_registerName("array"));

		say("[url] enumeratorAtURL: nil -> empty\n");
		return ((id (*)(id, SEL)) objc_msgSend)(empty, sel_registerName("objectEnumerator"));
	}
	return orig_enumerator(self, selector, url, keys, options, handler);
}

__attribute__((constructor))
static void
url_fix_init(void)
{
	Class cls;
	Method method;

	out = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (out < 0) {
		out = 2;
	}
	cls = objc_getClass("NSURL");
	if (cls == NULL) {
		return;
	}
	method = class_getInstanceMethod(cls, sel_registerName("initFileURLWithPath:"));
	if (method != NULL) {
		orig_init_path = (void *) method_setImplementation(method, (IMP) hook_init_path);
	}
	method = class_getInstanceMethod(cls, sel_registerName("initFileURLWithPath:isDirectory:"));
	if (method != NULL) {
		orig_init_path_dir = (void *) method_setImplementation(method, (IMP) hook_init_path_dir);
	}
	method = class_getClassMethod(cls, sel_registerName("fileURLWithPath:"));
	if (method != NULL) {
		orig_file_path = (void *) method_setImplementation(method, (IMP) hook_file_path);
	}
	{
		Class fm = objc_getClass("NSFileManager");
		Method enumerator = fm ? class_getInstanceMethod(fm,
		    sel_registerName("enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:")) : NULL;

		if (enumerator != NULL) {
			orig_enumerator = (void *) method_setImplementation(enumerator, (IMP) hook_enumerator);
		}
	}
	say("[url] NSURL nil-path fix in %s[%d]\n", getprogname(), getpid());
}

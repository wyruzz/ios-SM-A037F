/*
#include <stdlib.h>
#include <stdarg.h>
 * A minimal thread sampler for usermanagerd: no method hooks, no signal
 * handlers, nothing that can refuse to load.  It says, once at 30 s and once at
 * 90 s, what every thread of the process is doing -- enough to see which call
 * the daemon is stuck in when its clients wait for a reply.
 */
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

extern const char *getprogname(void);

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
	say("[um]   %s %#lx %s+%#lx\n", tag, (unsigned long) addr, name, off);
}

static void
sample(void)
{
	thread_act_array_t list = NULL;
	mach_msg_type_number_t count = 0;

	if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) {
		say("[um] task_threads failed\n");
		return;
	}
	say("[um] %u threads\n", (unsigned) count);
	for (mach_msg_type_number_t i = 0; i < count && i < 16; i++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		uintptr_t fp;

		if (thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t) &st, &sc) != KERN_SUCCESS) {
			mach_port_deallocate(mach_task_self(), list[i]);
			continue;
		}
		say("[um] T%u sp %#lx\n", i, (unsigned long) st.__sp);
		where("pc", (uintptr_t) st.__pc);
		where("lr", (uintptr_t) st.__lr);
		fp = (uintptr_t) st.__fp;
		for (int k = 0; k < 14 && fp != 0; k++) {
			uintptr_t next = ((const uintptr_t *) fp)[0];
			uintptr_t ret = ((const uintptr_t *) fp)[1];
			char tag[8];

			if (ret == 0) {
				break;
			}
			snprintf(tag, sizeof(tag), "f%d", k);
			where(tag, ret);
			if (next <= fp) {
				break;
			}
			fp = next;
		}
		mach_port_deallocate(mach_task_self(), list[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t) list, count * sizeof(*list));
}

static void *
um_probe_thread(void *arg __unused)
{
	sleep(30);
	sample();
	sleep(60);
	sample();
	return NULL;
}

__attribute__((constructor))
static void
um_probe_init(void)
{
	const char *name = getprogname();
	pthread_t thread;

	if (name == NULL || strcmp(name, "usermanagerd") != 0) {
		return;
	}
	out = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (out < 0) {
		out = 2;
	}
	say("[um] probe in usermanagerd[%d]\n", getpid());
	if (pthread_create(&thread, NULL, um_probe_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

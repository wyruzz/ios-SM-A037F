/*
 * libhakios.dylib: code of ours inside stock iOS programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of iOS 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "hakios.input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/redwood/lib/libhakios.gl.dylib"

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/*
 * Bring-up: say where a daemon's threads are, from inside it, when the log of the system
 * outside it cannot.  Used for lsd and cfprefsd while SpringBoard waits on them.
 */
static const char *probe_who = "?";

static void
probe_say(int fd, const char *format, ...)
{
	char line[320];
	char pre[80];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	{
		int pn = snprintf(pre, sizeof(pre), "[%s] ", probe_who);

		write(fd, pre, (size_t)pn);
	}
	write(fd, line, (size_t)n);
}

static void
probe_addr(int fd, const char *what, uintptr_t addr)
{
	Dl_info info;

	if (addr != 0 && dladdr((void *)addr, &info) && info.dli_fname != NULL) {
		const char *slash = strrchr(info.dli_fname, '/');

		probe_say(fd, "  %s %#lx %s+%#lx\n", what, (unsigned long)addr,
		    slash ? slash + 1 : info.dli_fname, (unsigned long)(addr - (uintptr_t)info.dli_fbase));
	} else if (addr != 0) {
		probe_say(fd, "  %s %#lx ?\n", what, (unsigned long)addr);
	}
}

static void
probe_threads(int fd)
{
	thread_act_array_t list = NULL;
	mach_msg_type_number_t count = 0;

	if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) {
		probe_say(fd, "  task_threads failed\n");
		return;
	}
	probe_say(fd, "  %u threads\n", (unsigned)count);
	for (mach_msg_type_number_t i = 0; i < count && i < 20; i++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		uintptr_t fp;

		if (thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) != KERN_SUCCESS) {
			mach_port_deallocate(mach_task_self(), list[i]);
			continue;
		}
		probe_say(fd, "  T%u pc %#lx lr %#lx fp %#lx\n", i,
		    (unsigned long)st.__pc, (unsigned long)st.__lr, (unsigned long)st.__fp);
		probe_addr(fd, "pc", (uintptr_t)st.__pc);
		probe_addr(fd, "lr", (uintptr_t)st.__lr);
		fp = (uintptr_t)st.__fp;
		for (int k = 0; k < 12 && fp != 0 && (fp & 7) == 0; k++) {
			uintptr_t next = ((const uintptr_t *)fp)[0];
			uintptr_t ret = ((const uintptr_t *)fp)[1];

			probe_addr(fd, "f", ret);
			if (next <= fp) {
				break;
			}
			fp = next;
		}
		mach_port_deallocate(mach_task_self(), list[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(*list));
}

static int probe_fd = 2;
static int probe_fix_um;

/* BOOL return for the UserManagement questions that make lsd ask usermanagerd for personas. */
static BOOL
probe_return_no(id self __unused, SEL cmd __unused)
{
	return NO;
}

static id
probe_return_nil(id self __unused, SEL cmd __unused)
{
	return nil;
}

static id
probe_return_empty(id self __unused, SEL cmd __unused)
{
	Class cls = objc_getClass("NSArray");

	return cls ? ((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("array")) : nil;
}

static void
user_manager_fixes(void)
{
	static int done;
	Class um;
	Method m;

	if (done || !probe_fix_um) {
		return;
	}
	um = objc_getClass("UMUserManager");
	if (um == Nil) {
		return;
	}
	done = 1;
	probe_say(probe_fd, "hakios probe: UMUserManager %p, isMultiUser/personasAreSupported\n", um);
	if ((m = class_getClassMethod(um, sel_registerName("isMultiUser"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_no);
		probe_say(probe_fd, "hakios probe: +[UMUserManager isMultiUser] -> NO\n");
	}
	if ((m = class_getInstanceMethod(um, sel_registerName("isMultiUser"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_no);
		probe_say(probe_fd, "hakios probe: -[UMUserManager isMultiUser] -> NO\n");
	}
	if ((m = class_getClassMethod(um, sel_registerName("personasAreSupported"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_no);
		probe_say(probe_fd, "hakios probe: +[UMUserManager personasAreSupported] -> NO\n");
	}
	if ((m = class_getInstanceMethod(um, sel_registerName("personasAreSupported"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_no);
		probe_say(probe_fd, "hakios probe: -[UMUserManager personasAreSupported] -> NO\n");
	}
	if ((m = class_getInstanceMethod(um, sel_registerName("currentPersona"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_nil);
		probe_say(probe_fd, "hakios probe: -[UMUserManager currentPersona] -> nil\n");
	}
	if ((m = class_getInstanceMethod(um, sel_registerName("currentUser"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_nil);
		probe_say(probe_fd, "hakios probe: -[UMUserManager currentUser] -> nil\n");
	}
	if ((m = class_getClassMethod(um, sel_registerName("currentUser"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_nil);
		probe_say(probe_fd, "hakios probe: +[UMUserManager currentUser] -> nil\n");
	}
	if ((m = class_getInstanceMethod(um, sel_registerName("listAllPersonaWithAttributes"))) != NULL) {
		method_setImplementation(m, (IMP)probe_return_empty);
		probe_say(probe_fd, "hakios probe: -[UMUserManager listAllPersonaWithAttributes] -> empty\n");
	}
}

static void
dump_um_methods(void)
{
	Class classes[3];
	classes[0] = objc_getClass("UMUserManager");
	classes[1] = objc_getClass("UMUser");
	classes[2] = objc_getClass("UMUserPersona");

	for (int c = 0; c < 3; c++) {
		Class cls = classes[c];
		if (cls == Nil) {
			continue;
		}
		for (int meta = 0; meta < 2; meta++) {
			Class owner = meta ? object_getClass(cls) : cls;
			unsigned count = 0;
			Method *list = class_copyMethodList(owner, &count);

			for (unsigned i = 0; i < count; i++) {
				SEL sel = method_getName(list[i]);
				IMP imp = method_getImplementation(list[i]);
				Dl_info info;

				if (dladdr((void *)imp, &info) && info.dli_fname != NULL &&
				    strstr(info.dli_fname, "UserManagement") != NULL) {
					probe_say(probe_fd, "hakios probe: UM method %c[%s %s] +%#lx\n",
					    meta ? '+' : '-', class_getName(cls), sel_getName(sel),
					    (unsigned long)((uintptr_t)imp - (uintptr_t)info.dli_fbase));
				}
			}
			free(list);
		}
	}
}

static void
probe_signal(int sig, siginfo_t *si __unused, void *vctx)
{
	ucontext_t *uc = (ucontext_t *)vctx;
	uintptr_t pc = 0, lr = 0, sp = 0;

	if (uc != NULL) {
		pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
	}
	probe_say(probe_fd, "hakios probe: SIGNAL %d pc %#lx lr %#lx sp %#lx slide %#lx\n",
	    sig, (unsigned long)pc, (unsigned long)lr, (unsigned long)sp, (unsigned long)slide);
	if (uc != NULL) {
		for (int r = 0; r < 29; r++) {
			uintptr_t v = (uintptr_t)uc->uc_mcontext->__ss.__x[r];

			if (v != 0) {
				probe_say(probe_fd, "  x%d %#lx\n", r, (unsigned long)v);
			}
			if (v > 0x100000000UL && v < 0x300000000UL) {
				char str[96];
				int k;

				for (k = 0; k < (int)sizeof(str) - 1; k++) {
					char c = ((const char *)v)[k];

					if (c == 0) {
						break;
					}
					if (c < 32 || c > 126) {
						k = 0;
						break;
					}
					str[k] = c;
				}
				if (k >= 4) {
					str[k] = 0;
					probe_say(probe_fd, "  str x%d \"%s\"\n", r, str);
				}
			}
		}
	}
	if (sp != 0) {
		for (int i = 0; i < 64; i++) {
			uintptr_t w = ((const uintptr_t *)sp)[i];

			if (w > 0x100000000UL && w < 0x300000000UL) {
				char sbuf[128];
				vm_size_t got = 0;

				if (vm_read_overwrite(mach_task_self(), (vm_address_t)w, sizeof(sbuf),
				    (vm_address_t)sbuf, &got) == KERN_SUCCESS) {
					int k;

					for (k = 0; k < (int)got && k < (int)sizeof(sbuf) - 1; k++) {
						if (sbuf[k] == 0) {
							break;
						}
						if (sbuf[k] < 32 || sbuf[k] > 126) {
							k = 0;
							break;
						}
					}
					if (k >= 4) {
						sbuf[k] = 0;
						probe_say(probe_fd, "  stk[%d] %#lx \"%s\"\n", i,
						    (unsigned long)w, sbuf);
					}
				}
			}
		}
	}
	if (sig == SIGTRAP && uc != NULL) {
		unsigned po = (unsigned)(pc & 0xfff);
		uintptr_t npc = 0;

		/*
		 * libsystem_trace's os_log-fault path ends in brk #1; go to its
		 * epilogue instead, so the daemon can keep answering.
		 */
		if (po == 0x9d0) {
			npc = pc - 0x18;
		} else if (po == 0x9e0) {
			npc = pc - 0x28;
		} else if (po == 0x9f0) {
			npc = pc - 0x38;
		} else if (po == 0xa00) {
			npc = pc - 0x48;
		}
		if (npc != 0) {
			uc->uc_mcontext->__ss.__pc = npc;
			probe_say(probe_fd, "hakios probe: skipped os_log brk to %#lx\n",
			    (unsigned long)npc);
			return;
		}
	}
	if (sp != 0) {
		for (int i = 0; i < 32; i++) {
			uintptr_t w = ((const uintptr_t *)sp)[i];

			if (w > 0x100000000UL && w < 0x240000000UL) {
				probe_say(probe_fd, "  sig stack %d %#lx unslid %#lx\n", i,
				    (unsigned long)w, (unsigned long)(w - slide));
			}
		}
	}
	_exit(1);
}

static BOOL
rbs_acquire_yes(id self __unused, SEL cmd __unused, void *error)
{
	if (error != NULL) {
		*(void **)error = NULL;
	}
	return YES;
}

static void
rbs_acquire_noop(id self __unused, SEL cmd __unused, id handler __unused)
{
}

/*
 * usermanagerd dies on a brk in libsystem_trace's os_log-fault path.  Let
 * that path return through its epilogue instead of trapping: the daemon can
 * then answer the user/persona questions runningboardd and lsd are waiting on.
 */
static void
patch_trace_brk(void)
{
	uintptr_t base = 0;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "libsystem_trace.dylib") != NULL) {
			base = (uintptr_t)_dyld_get_image_header(i);
			break;
		}
	}
	if (base == 0) {
		return;
	}
	{
		static const uint32_t brks[] = { 0x149d0, 0x149e0, 0x149f0, 0x14a00 };
		uintptr_t epilogue = base + 0x149b8;

		for (unsigned i = 0; i < sizeof(brks) / sizeof(brks[0]); i++) {
			uintptr_t at = base + brks[i];
			uintptr_t page = at & ~0xfffUL;
			int32_t off = (int32_t)((epilogue - at) / 4);
			uint32_t insn = 0x14000000 | ((uint32_t)off & 0x03ffffffu);
			uint32_t *p = (uint32_t *)at;

			if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
				probe_say(probe_fd, "trace brk patch: mprotect failed\n");
				break;
			}
			*p = insn;
			sys_icache_invalidate((void *)at, 4);
		}
		probe_say(probe_fd, "trace brk patch: os_log fault returns at %#lx\n", (unsigned long)base);
	}
}

static void
patch_rbs_monitor_code(void)
{
	uintptr_t base = 0;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "RunningBoardServices") != NULL) {
			base = (uintptr_t)_dyld_get_image_header(i);
			break;
		}
	}
	if (base == 0) {
		return;
	}
	{
		uintptr_t entry = base + 0xb9e0;        /* +[RBSProcessMonitor monitorWithConfiguration:] */
		uintptr_t page = entry & ~0xfffUL;
		uint32_t *p = (uint32_t *)entry;

		if (mprotect((void *)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
			probe_say(probe_fd, "RBS code patch: mprotect failed\n");
			return;
		}
		p[0] = 0x58000050;                      /* ldr x16, #8 */
		p[1] = 0xd61f0200;                      /* br x16 */
		((uint64_t *)(p + 2))[0] = (uint64_t)(uintptr_t)probe_return_nil;
		sys_icache_invalidate((void *)entry, 16);
		probe_say(probe_fd, "RBS code patch: monitorWithConfiguration -> nil at %#lx\n",
		    (unsigned long)entry);
	}
}

static void
hook_rbs_assertion(void)
{
	Class cls = objc_getClass("RBSAssertion");
	Method m;

	if (cls == Nil) {
		return;
	}
	if ((m = class_getInstanceMethod(cls, sel_registerName("acquireWithError:"))) != NULL) {
		method_setImplementation(m, (IMP)rbs_acquire_yes);
		probe_say(probe_fd, "RBSAssertion acquireWithError -> YES\n");
	}
	if ((m = class_getInstanceMethod(cls, sel_registerName("acquireWithInvalidationHandler:"))) != NULL) {
		method_setImplementation(m, (IMP)rbs_acquire_noop);
		probe_say(probe_fd, "RBSAssertion acquireWithInvalidationHandler -> return\n");
	}
}

/*
 * backboardd, at its [SystemShell] start watchdog for %{public}s: iOS 16.5
 * (20F66, the iPhone X's backboardd), the function arms the shell watchdog,
 * which fires systemIdleSleepInterval (3.00 s) after the shell's check-in and
 * invalidates the shell's connection -- the process, running, then exits.
 * Here it does not respond in 3 s, and neither does iOS's reason for it matter
 * much to us: do not arm the watchdog at all.
 */
static void
patch_backboard_shell_watchdog(void)
{
	uintptr_t base = 0, entry, page;
	kern_return_t kr;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "libexec/backboardd") != NULL) {
			base = (uintptr_t)_dyld_get_image_header(i);
			break;
		}
	}
	if (base == 0) {
		say("hakios: backboardd image not found for the shell watchdog\n");
		return;
	}
	entry = base + 0x1fb68;
	page = entry & ~0xfffUL;
	kr = vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: shell watchdog: vm_protect kr %d\n", kr);
		return;
	}
	*(uint32_t *)entry = 0xd65f03c0;        /* ret */
	sys_icache_invalidate((void *)entry, 4);
	vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
	    VM_PROT_READ | VM_PROT_EXECUTE);
	say("hakios: backboardd shell watchdog setter -> ret at %#lx\n", (unsigned long)entry);
}

/*
 * lsd asks InstalledContentLibrary's _GetEligiblePersonaForBundle for a
 * persona, and that synchronous UserManagement XPC is what holds the
 * LaunchServices lock SpringBoard waits on.  This port has no personas:
 * return nil from that function instead.
 */
static void
patch_persona_lookup(void)
{
	uintptr_t base = 0;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "InstalledContentLibrary") != NULL) {
			base = (uintptr_t)_dyld_get_image_header(i);
			break;
		}
	}
	if (base == 0) {
		probe_say(probe_fd, "persona patch: InstalledContentLibrary not found\n");
		return;
	}
	{
		uintptr_t entry = base + 0xce98;        /* _GetEligiblePersonaForBundle */
		uintptr_t page = entry & ~0xfffUL;
		uint32_t *p = (uint32_t *)entry;

		{
			kern_return_t kr = vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
			    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

			if (kr != KERN_SUCCESS) {
				probe_say(probe_fd, "persona patch: vm_protect RW failed kr %d\n", kr);
				return;
			}
		}
		p[0] = 0xd2800000;                      /* mov x0, #0 */
		p[1] = 0xd65f03c0;                      /* ret */
		sys_icache_invalidate((void *)entry, 8);
		vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
		    VM_PROT_READ | VM_PROT_EXECUTE);
		probe_say(probe_fd, "persona patch: _GetEligiblePersonaForBundle -> nil at %#lx\n",
		    (unsigned long)entry);
	}
}

static void
dump_rbs_methods(void)
{
	static const char *const names[] = {
		"RBSAssertion", "RBSConnection", "RBSProcessMonitor", "RBSProcessHandle",
		"RBSService", "RBSProcessIdentity", "RBSProcessState"
	};

	for (unsigned c = 0; c < sizeof(names) / sizeof(names[0]); c++) {
		Class cls = objc_getClass(names[c]);

		if (cls == Nil) {
			continue;
		}
		for (int meta = 0; meta < 2; meta++) {
			Class owner = meta ? object_getClass(cls) : cls;
			unsigned count = 0;
			Method *list = class_copyMethodList(owner, &count);

			for (unsigned i = 0; i < count; i++) {
				SEL sel = method_getName(list[i]);
				IMP imp = method_getImplementation(list[i]);
				Dl_info info;

				if (dladdr((void *)imp, &info) && info.dli_fname != NULL &&
				    strstr(info.dli_fname, "RunningBoardServices") != NULL) {
					probe_say(probe_fd, "RBS method %c[%s %s] +%#lx\n",
					    meta ? '+' : '-', names[c], sel_getName(sel),
					    (unsigned long)((uintptr_t)imp - (uintptr_t)info.dli_fbase));
				}
			}
			free(list);
		}
	}
}

static void
restart_once(const char *marker, int seconds)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC),
	    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		int fd;

		if (access(marker, F_OK) == 0) {
			return;
		}
		fd = open(marker, O_CREAT | O_WRONLY, 0644);
		if (fd < 0) {
			probe_say(probe_fd, "restart: cannot write %s\n", marker);
			return;
		}
		close(fd);
		probe_say(probe_fd, "restart: %s, killing %d\n", marker, getpid());
		kill(getpid(), SIGKILL);
	});
}

static void
probe_start(const char *name)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	if (fd < 0) {
		fd = 2;
	}
	probe_fd = fd;
	probe_who = name;
	for (int n = 1; n <= 3; n++) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 3000 * NSEC_PER_MSEC),
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			dump_rbs_methods();
		});
	}
	if (strcmp(name, "backboardd") == 0) {
		patch_backboard_shell_watchdog();
		for (int n = 1; n <= 8; n++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 1000 * NSEC_PER_MSEC),
			    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				hook_rbs_assertion();
			});
		}
	}
	/* (do not swizzle UMUserManager in lsd: doing that on a background queue
	 * while lsd is calling into it makes lsd die of SIGSEGV) */
	probe_fix_um = strcmp(name, "runningboardd") == 0;
	probe_say(fd, "hakios probe: in %s[%d]\n", name, getpid());
	if (strcmp(name, "usermanagerd") == 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = probe_signal;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGTRAP, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		probe_say(fd, "hakios probe: signal handlers in\n");
	}
	if (probe_fix_um) {
		for (int n = 1; n <= 4; n++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 3000 * NSEC_PER_MSEC),
			    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				dump_um_methods();
			});
		}
	}
	if (strcmp(name, "usermanagerd") == 0) {
		for (int n = 1; n <= 8; n++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 200 * NSEC_PER_MSEC),
			    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				probe_say(fd, "hakios probe: early %d\n", n);
				probe_threads(fd);
			});
		}
	}
	{
		int samples = strcmp(name, "lsd") == 0 ? 60 : 12;

		for (int n = 1; n <= samples; n++) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)n * 10 * NSEC_PER_SEC),
		    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				user_manager_fixes();
				probe_say(fd, "hakios probe: sample %d\n", n);
				probe_threads(fd);
			});
		}
	}
}

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("hakios: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

/*
 * backboardd asks Security for an entitlement of another process (SpringBoard's
 * com.apple.backboard.client) and refuses its display overlay when the answer
 * is nil; on this port SpringBoard's own signature does not load, so it always
 * is.  Answer that one question ourselves, from inside backboardd.
 */
typedef struct __SecTask *SecTaskRef;
extern CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error);

static CFTypeRef (*original_sec_task_value)(SecTaskRef, CFStringRef, CFErrorRef *);

static CFTypeRef
sec_task_value(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error)
{
	if (entitlement != NULL &&
	    (CFStringCompare(entitlement, CFSTR("com.apple.backboard.client"), 0) == kCFCompareEqualTo ||
	     CFStringCompare(entitlement, CFSTR("com.apple.springboard-ui.client"), 0) == kCFCompareEqualTo ||
	     CFStringCompare(entitlement, CFSTR("com.apple.corebrightness"), 0) == kCFCompareEqualTo ||
	     CFStringCompare(entitlement, CFSTR("com.apple.corebrightness.client"), 0) == kCFCompareEqualTo)) {
		return CFRetain(kCFBooleanTrue);        /* the caller releases what it gets */
	}
	return original_sec_task_value(task, entitlement, error);
}

__attribute__((used)) static struct {
	const void *replacement;
	const void *replacee;
} hakios_interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)sec_task_value, (const void *)SecTaskCopyValueForEntitlement },
};

static id (*original_file_url)(id, SEL, id);

static id
file_url_with_path(id self, SEL _cmd, id path)
{
	if (path == nil) {
		say("hakios: window server: a nil path for a file URL (the container query was refused); using /private/var/tmp\n");
		path = (id)CFSTR("/private/var/tmp");
	}
	return original_file_url(self, _cmd, path);
}

void
window_server_fixes(void)
{
	/*
	 * The window server asks containermanagerd for the system container of
	 * com.apple.backboardd; on this port the answer is NOT_CODESIGNED (the
	 * program's entitlements are not loaded), the path comes back nil, and
	 * -[NSURL initFileURLWithPath:] raises and kills the server before it
	 * draws anything.  A nil path here can only end in that exception, so
	 * replace it with a directory that exists and is writable.
	 */
	{
		Class url = objc_getClass("NSURL");
		Method method = url != Nil ? class_getInstanceMethod(url, sel_registerName("initFileURLWithPath:")) : NULL;

		if (method != NULL) {
			original_file_url = (id (*)(id, SEL, id))method_getImplementation(method);
			method_setImplementation(method, (IMP)file_url_with_path);
			say("hakios: window server: nil file paths for NSURL replaced\n");
		}
	}
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the display's backlight -------------------------------------------------------- */

/*
 * No digitizer turns up, so backboardd turns the backlight off for good
 * ("DisplayBrightnessFactor to:0 ... permanently:YES") and the Apple logo it
 * then draws is never seen.  Backboardd talks to the BrightnessSystem; we can
 * do the same from inside it and put the factor back.
 */
static void *
backlight_thread(void *arg)
{
	(void)arg;
	CFNumberRef factor, level;
	Class cls;
	id client;
	BOOL (*set_property)(id, SEL, id, id);
	const char *keys[] = { "DisplayBrightnessFactor", "DisplayBrightness" };
	float values[] = { 1.0f, 0.5f };

	sleep(20);                              /* let backboardd start its BrightnessSystem */
	cls = objc_getClass("BrightnessSystemClient");
	if (cls == NULL) {
		say("hakios: backlight: no BrightnessSystemClient class\n");
		return NULL;
	}
	client = ((id (*)(id, SEL))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("alloc")),
	    sel_registerName("init"));
	if (client == nil) {
		say("hakios: backlight: no client\n");
		return NULL;
	}
	set_property = (BOOL (*)(id, SEL, id, id))objc_msgSend;
	for (int round = 0; round < 120; round++) {
		for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			CFStringRef key = CFStringCreateWithCString(NULL, keys[k], kCFStringEncodingUTF8);
			BOOL ok;

			if (k == 0) {
				factor = CFNumberCreate(NULL, kCFNumberFloat32Type, &values[k]);
			} else {
				level = CFNumberCreate(NULL, kCFNumberFloat32Type, &values[k]);
				factor = level;
			}
			ok = set_property(client, sel_registerName("setProperty:forKey:"), (id)factor, (id)key);
			if (round < 3) {
				say("hakios: backlight: %s -> %d\n", keys[k], ok);
			}
			CFRelease(factor);
			CFRelease(key);
		}
		sleep(5);
	}
	say("hakios: backlight: stopped forcing\n");
	return NULL;
}

/*
 * Backboardd's own backlight setter is the one call that certainly reaches the
 * hardware; our BrightnessSystemClient writes do not (they never show up in its
 * log).  Intercept the setters on BLControl and keep the factor away from zero.
 */
struct backlight_hook {
	Method method;
	IMP original;
};

static struct backlight_hook backlight_hooks[8];
static int backlight_hook_count;

static void
backlight_set(id self, SEL _cmd, float factor, float duration)
{
	static int said;

	for (int i = 0; i < backlight_hook_count; i++) {
		if (backlight_hooks[i].method == NULL || sel_isEqual(_cmd, method_getName(backlight_hooks[i].method)) == false) {
			continue;
		}
		if (factor <= 0.0f) {
			if (said++ < 8) {
				say("hakios: backlight: %s %g -> 0.5\n", sel_getName(_cmd), factor);
			}
			factor = 0.5f;
		}
		((void (*)(id, SEL, float, float))backlight_hooks[i].original)(self, _cmd, factor, duration);
		return;
	}
}

static void
backlight_clamp(void)
{
	Class cls = objc_getClass("BLControl");
	unsigned count = 0;
	Method *methods;
	int dumped = 0;

	if (cls == NULL) {
		say("hakios: backlight: no BLControl class\n");
		return;
	}
	methods = class_copyMethodList(cls, &count);
	for (unsigned i = 0; i < count && backlight_hook_count < 8; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (strstr(name, "actor") == NULL && strstr(name, "ightness") == NULL && strstr(name, "lank") == NULL) {
			continue;
		}
		if (dumped++ < 30) {
			say("hakios: BLControl method %s\n", name);
		}
		if (strncmp(name, "set", 3) == 0 && strchr(name, ':') != NULL &&
		    (strstr(name, "actor") != NULL || strstr(name, "ightness") != NULL)) {
			backlight_hooks[backlight_hook_count].method = methods[i];
			backlight_hooks[backlight_hook_count].original = method_setImplementation(methods[i], (IMP)backlight_set);
			backlight_hook_count++;
			say("hakios: backlight: hooked %s\n", name);
		}
	}
	free(methods);
}

/*
 * Backboardd keeps blanking the display ("Set Blanked: YES", factor 0) because
 * the system shell never gets far enough to own it, and a blanked display is
 * one CoreAnimation does not draw into.  Two hooks inside this process: never
 * let the BrightnessSystem factor reach zero, and never let a "setBlanked:"
 * method keep the display blanked.
 */
static void (*original_set_property)(id, SEL, id, id);
static int backlight_zero_said;

static void
brightness_set_property(id self, SEL selector, id value, id key)
{
	if (key != NULL && value != NULL && CFGetTypeID((CFTypeRef) key) == CFStringGetTypeID() &&
	    CFEqual((CFTypeRef) key, CFSTR("DisplayBrightnessFactor")) &&
	    CFGetTypeID((CFTypeRef) value) == CFNumberGetTypeID()) {
		double factor = 0;

		CFNumberGetValue((CFNumberRef) value, kCFNumberDoubleType, &factor);
		if (factor <= 0.0) {
			float half = 0.5f;
			CFNumberRef replacement = CFNumberCreate(NULL, kCFNumberFloat32Type, &half);

			if (backlight_zero_said++ < 8) {
				say("hakios: backlight: %s 0 -> 0.5\n", sel_getName(selector));
			}
			if (replacement != NULL) {
				original_set_property(self, selector, (id) replacement, key);
				CFRelease(replacement);
				return;
			}
		}
	}
	original_set_property(self, selector, value, key);
}

struct blank_hook {
	Method method;
	IMP original;
};

static struct blank_hook blank_hooks[8];
static int blank_hook_count, blank_said;

static void
no_blank_hook(id self, SEL selector, BOOL blanked)
{
	for (int i = 0; i < blank_hook_count; i++) {
		if (sel_isEqual(selector, method_getName(blank_hooks[i].method))) {
			if (blanked && blank_said++ < 8) {
				say("hakios: %s: blanked YES -> NO\n", sel_getName(selector));
			}
			((void (*)(id, SEL, BOOL)) blank_hooks[i].original)(self, selector, NO);
			return;
		}
	}
}

static void
display_blank_fixes(void)
{
	Class cls = objc_getClass("BrightnessSystemClient");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("setProperty:forKey:")) : NULL;
	int classes = 0, found_blank = 0;

	if (method != NULL) {
		original_set_property = (void *) method_setImplementation(method, (IMP) brightness_set_property);
		say("hakios: backlight: BrightnessSystemClient setProperty hooked\n");
	}
	classes = objc_getClassList(NULL, 0);
	if (classes > 0) {
		Class *list = calloc((size_t) classes, sizeof(*list));

		if (list != NULL) {
			classes = objc_getClassList(list, classes);
			for (int i = 0; i < classes && blank_hook_count < 8; i++) {
				unsigned count = 0;
				Method *methods = class_copyMethodList(list[i], &count);

				for (unsigned m = 0; m < count && blank_hook_count < 8; m++) {
					const char *name = sel_getName(method_getName(methods[m]));

					if (strstr(name, "lank") != NULL && strncmp(name, "set", 3) == 0 && strchr(name, ':') != NULL) {
						blank_hooks[blank_hook_count].method = methods[m];
						blank_hooks[blank_hook_count].original = method_setImplementation(methods[m],
						    (IMP) no_blank_hook);
						found_blank++;
						say("hakios: blank hook: -[%s %s]\n", class_getName(list[i]), name);
						blank_hook_count++;
					}
				}
				free(methods);
			}
			free(list);
		}
	}
	if (found_blank == 0) {
		say("hakios: blank hooks: none found in %d classes\n", classes);
	}
}

static void
backlight_start(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, backlight_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			usleep(200000);         /* the server may still be settling: try again */
			continue;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void *
input_thread(__unused void *arg)
{
	/* the type that may create a virtual service is not documented: try them all */
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED, 1, 2, 5, 6, 7, 8 };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		/*
		 * No kernel touch device yet, but the event server still needs a
		 * digitizer: without one backboardd keeps the display's backlight off
		 * ("still waiting for a main screen digitizer").  Make the service
		 * anyway; only the reading of samples needs the device.
		 */
		say("hakios: input: %s: %s (the digitizer service stays up)\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		say("hakios: input: client type %d -> %p\n", types[i], (void *)client);
		if (client == NULL) {
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("hakios: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}
	if (device < 0) {
		for (;;) {
			pause();        /* keep the virtual digitizer service registered */
		}
	}

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- lsd: the user-management lookup that never answers --------------------------------------- */

/*
 * lsd, holding the LaunchServices lock, calls into UserManagement (a Swift
 * framework) to find the persona of a bundle; that call goes over a synchronous
 * XPC to usermanagerd, which does not answer, so every process that asks
 * LaunchServices for anything stops behind that lock -- SpringBoard's
 * UIApplicationMain among them.
 *
 * Patch the one call in InstalledContentLibrary (+0xd070) to return nil: lsd
 * treats the bundle as having no persona, as it does for a bundle outside any
 * user, and goes on.
 */
static void
lsd_persona_call_fix(void)
{
	uintptr_t base = 0, entry, page;
	kern_return_t kr;

	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);

		if (name != NULL && strstr(name, "InstalledContentLibrary.framework/") != NULL) {
			base = (uintptr_t)_dyld_get_image_header(i);
			break;
		}
	}
	if (base == 0) {
		say("hakios: lsd: InstalledContentLibrary not found\n");
		return;
	}
	entry = base + 0xd070;
	page = entry & ~0xfffUL;
	kr = vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: lsd persona: vm_protect kr %d\n", kr);
		return;
	}
	*(uint32_t *)entry = 0xd2800000;        /* mov x0, #0 -- the call returns nil */
	sys_icache_invalidate((void *)entry, 4);
	vm_protect(mach_task_self(), (vm_address_t)page, 0x2000, FALSE,
	    VM_PROT_READ | VM_PROT_EXECUTE);
	say("hakios: lsd: persona lookup call at %#lx -> nil\n", (unsigned long)entry);
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
static void
opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return;                                 /* (an image built without it) */
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		functions_said_yes(arguments);
		compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				wanted = argument[3] != '0';
				told = 1;
			}
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("hakios: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(HAKIOS_GL, RTLD_NOW)) == NULL) {
		say("hakios: %s: %s\n", HAKIOS_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "hakios_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "hakios_gl_function_refused");
}

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "backboardd") == 0) {
		char arguments[1024] = "";

		unlink("/private/var/tmp/.hakios-lsd-restart");
		unlink("/private/var/tmp/.hakios-sb-restart");
		size_t size = sizeof(arguments) - 1;

		/*
		 * Boot-arg ca_vbl=1: CoreAnimation's frame clock from the display driver's refresh
		 * notifications. QuartzCore takes them (IOMobileFramebufferEnableVSyncNotifications) only on
		 * the phones with an A7 to A10, or when CA_FORCE_TIMER is in the environment
		 * (CADeviceUseVBL()); on the iPhone X it runs a timer of its own instead (CADisplayTimer),
		 * whose phase it would set from the frame timings of a display driver this one is not.
		 */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				setenv(setting, equals + 1, 1);
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		opengl_es();
		window_server_fixes();
		/*
		 * Wait for our touch screen service instead of the missing ambient light sensor:
		 * backboardd only takes a digitizer that is already there when its touch
		 * processing starts, and without one it turns the backlight off for good.
		 */
		brightness_fixes();
		backlight_clamp();
		display_blank_fixes();
		input_start();
		backlight_start();
		probe_start(name);
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	} else if (strcmp(name, "lsd") == 0 || strcmp(name, "cfprefsd") == 0 ||
	    strcmp(name, "usermanagerd") == 0 || strcmp(name, "runningboardd") == 0) {
		if (strcmp(name, "lsd") == 0) {
			lsd_persona_call_fix();
		}
		probe_start(name);
	}
}

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>

#if WAVM_ENABLE_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include "POSIXPrivate.h"
#include "WAVM/Inline/Assert.h"
#include "WAVM/Inline/BasicTypes.h"
#include "WAVM/Inline/Config.h"
#include "WAVM/Inline/Errors.h"
#include "WAVM/Inline/Lock.h"
#include "WAVM/Platform/Exception.h"
#include "WAVM/Platform/Intrinsic.h"
#include "WAVM/Platform/Memory.h"
#include "WAVM/Platform/Mutex.h"
#include "WAVM/Platform/Thread.h"

#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef __linux__
#define MAP_STACK_FLAGS (MAP_STACK)
#else
#define MAP_STACK_FLAGS 0
#endif

#if WAVM_ENABLE_ASAN
extern "C" void __asan_get_shadow_mapping(Uptr* shadow_scale, Uptr* shadow_offset);
extern "C" Uptr __asan_stack_malloc(Uptr size, Uptr real_stack);
extern "C" void __asan_stack_free(Uptr ptr, Uptr size, Uptr real_stack);
#endif

using namespace WAVM;
using namespace WAVM::Platform;

struct Platform::Thread
{
	pthread_t id;
};

struct CreateThreadArgs
{
	I64 (*entry)(void*);
	void* entryArgument;
};

struct ForkThreadArgs
{
	ExecutionContext forkContext;
	Platform::Mutex forkedStackMutex;
	U8* threadEntryFramePointer;
};

enum
{
	sigAltStackNumBytes = 65536
};

#define ALLOCATE_SIGALTSTACK_ON_MAIN_STACK 0

static bool isAlignedLog2(void* pointer, Uptr alignmentLog2)
{
	return !(reinterpret_cast<Uptr>(pointer) & ((Uptr(1) << alignmentLog2) - 1));
}

void SigAltStack::deinit()
{
	if(base)
	{
		// Disable the sig alt stack.
		// According to the docs, ss_size is ignored if SS_DISABLE is set, but MacOS returns an
		// ENOMEM error if ss_size is too small regardless of whether SS_DISABLE is set.
		stack_t disableAltStack;
		memset(&disableAltStack, 0, sizeof(stack_t));
		disableAltStack.ss_flags = SS_DISABLE;
		disableAltStack.ss_size = sigAltStackNumBytes;
		errorUnless(!sigaltstack(&disableAltStack, nullptr));

		// Free the alt stack's memory.
		if(!ALLOCATE_SIGALTSTACK_ON_MAIN_STACK) { errorUnless(!munmap(base, sigAltStackNumBytes)); }
		else
		{
			U8* signalStackMaxAddr = base + sigAltStackNumBytes;
			if(mprotect(signalStackMaxAddr, Uptr(1) << getPageSizeLog2(), PROT_READ | PROT_WRITE)
			   != 0)
			{
				Errors::fatalf("mprotect(0x%" PRIxPTR ", %" PRIuPTR
							   ", PROT_READ | PROT_WRITE) returned %i.\n",
							   reinterpret_cast<Uptr>(signalStackMaxAddr),
							   Uptr(1) << getPageSizeLog2(),
							   errno);
			}
		}
		base = nullptr;
	}
}

NO_ASAN static void touchStackPages(U8* minAddr, Uptr numBytesPerPage)
{
	U8 sum = 0;
	while(true)
	{
		volatile U8* touchAddr = (volatile U8*)alloca(numBytesPerPage / 2);
		sum += *touchAddr;
		if(touchAddr < minAddr + numBytesPerPage) { break; }
	}
}

void SigAltStack::init()
{
	if(!base)
	{
		// Allocate a stack to use when handling signals, so stack overflow can be handled safely.
		if(!ALLOCATE_SIGALTSTACK_ON_MAIN_STACK)
		{
			base = (U8*)mmap(nullptr,
							 sigAltStackNumBytes,
							 PROT_READ | PROT_WRITE,
							 MAP_PRIVATE | MAP_ANONYMOUS,
							 -1,
							 0);
		}
		else
		{
			const Uptr pageSizeLog2 = getPageSizeLog2();
			const Uptr numBytesPerPage = Uptr(1) << pageSizeLog2;

			// Use the top of the thread's normal stack to handle signals: just protect a page below
			// the pages used by the sigaltstack that will catch stack overflows before they
			// overwrite the sigaltstack.
			U8* stackMinGuardAddr = nullptr;
			U8* stackMinAddr = nullptr;
			U8* stackMaxAddr = nullptr;
			getCurrentThreadStack(stackMinGuardAddr, stackMinAddr, stackMaxAddr);

			// Touch each stack page from bottom to top to ensure that it has been mapped: Linux and
			// possibly other OSes lazily grow the stack mapping as a guard page is hit.
			touchStackPages(stackMinAddr, numBytesPerPage);

			// Protect a page in between the sigaltstack and the rest of the stack.
			U8* signalStackMaxAddr = stackMinAddr + sigAltStackNumBytes;
			errorUnless(isAlignedLog2(signalStackMaxAddr, pageSizeLog2));
			if(mprotect(signalStackMaxAddr, numBytesPerPage, PROT_NONE) != 0)
			{
				Errors::fatalf("mprotect(0x%" PRIxPTR ", %" PRIuPTR ", PROT_NONE) returned %i.\n",
							   reinterpret_cast<Uptr>(signalStackMaxAddr),
							   numBytesPerPage,
							   errno);
			}

			base = stackMinAddr;
		}

		errorUnless(base != MAP_FAILED);
		stack_t sigAltStackInfo;
		sigAltStackInfo.ss_size = sigAltStackNumBytes;
		sigAltStackInfo.ss_sp = base;
		sigAltStackInfo.ss_flags = 0;
		errorUnless(!sigaltstack(&sigAltStackInfo, nullptr));
	}
}

thread_local SigAltStack Platform::sigAltStack;

struct ThreadEntryContext
{
	jmp_buf exitJump;
	I64 exitCode;
	U8* framePointer = nullptr;
};

static thread_local ThreadEntryContext* threadEntryContext = nullptr;

static Uptr getNumStackBytesRLimit()
{
	// Use getrlimit to find the maximum size of the stack instead of the current.
	struct rlimit stackLimit;
	getrlimit(RLIMIT_STACK, &stackLimit);
	return stackLimit.rlim_cur;
}

static void getThreadStack(pthread_t thread, U8*& outMinGuardAddr, U8*& outMinAddr, U8*& outMaxAddr)
{
	Uptr numGuardBytes = 0;
#ifdef __linux__
	// Linux uses pthread_getattr_np/pthread_attr_getstack, and returns a pointer to the minimum
	// address of the stack.
	pthread_attr_t threadAttributes;
	memset(&threadAttributes, 0, sizeof(threadAttributes));
	errorUnless(!pthread_getattr_np(thread, &threadAttributes));
	Uptr numStackBytes;
	errorUnless(!pthread_attr_getstack(&threadAttributes, (void**)&outMinAddr, &numStackBytes));
	errorUnless(!pthread_attr_getguardsize(&threadAttributes, &numGuardBytes));
	errorUnless(!pthread_attr_destroy(&threadAttributes));
	outMaxAddr = outMinAddr + numStackBytes;
#elif defined(__APPLE__)
	// MacOS uses pthread_get_stackaddr_np, and returns a pointer to the maximum address of the
	// stack.
	outMaxAddr = (U8*)pthread_get_stackaddr_np(thread);
	outMinAddr = outMaxAddr - getNumStackBytesRLimit();
	numGuardBytes = Uptr(1) << getPageSizeLog2();
#elif defined(__WAVIX__)
	Errors::fatal("getCurrentThreadStack is unimplemented on Wavix.");
#else
#error unsupported platform
#endif

	outMinGuardAddr = outMinAddr - numGuardBytes;
}

void Platform::getCurrentThreadStack(U8*& outMinGuardAddr, U8*& outMinAddr, U8*& outMaxAddr)
{
	getThreadStack(pthread_self(), outMinGuardAddr, outMinAddr, outMaxAddr);
}

NO_ASAN static void* createThreadEntry(void* argsVoid)
{
	std::unique_ptr<CreateThreadArgs> args((CreateThreadArgs*)argsVoid);

	sigAltStack.init();

	ThreadEntryContext localThreadEntryContext;
	localThreadEntryContext.framePointer = getStackPointer();
	localThreadEntryContext.exitCode = -1;
	if(!sigsetjmp(localThreadEntryContext.exitJump, 1))
	{
		threadEntryContext = &localThreadEntryContext;
		localThreadEntryContext.exitCode = (*args->entry)(args->entryArgument);
	}

	sigAltStack.deinit();

	return reinterpret_cast<void*>(localThreadEntryContext.exitCode);
}

Platform::Thread* Platform::createThread(Uptr numStackBytes,
										 I64 (*threadEntry)(void*),
										 void* argument)
{
	auto thread = new Thread;
	auto createArgs = new CreateThreadArgs;
	createArgs->entry = threadEntry;
	createArgs->entryArgument = argument;

	numStackBytes = std::max(numStackBytes, getNumStackBytesRLimit());

	pthread_attr_t threadAttr;
	errorUnless(!pthread_attr_init(&threadAttr));
	errorUnless(!pthread_attr_setstacksize(&threadAttr, numStackBytes));

	// Create a new pthread.
	errorUnless(!pthread_create(&thread->id, &threadAttr, createThreadEntry, createArgs));
	errorUnless(!pthread_attr_destroy(&threadAttr));

	return thread;
}

void Platform::detachThread(Thread* thread)
{
	errorUnless(!pthread_detach(thread->id));
	delete thread;
}

I64 Platform::joinThread(Thread* thread)
{
	void* returnValue = nullptr;
	errorUnless(!pthread_join(thread->id, &returnValue));
	delete thread;
	return reinterpret_cast<I64>(returnValue);
}

void Platform::exitThread(I64 argument)
{
	wavmAssert(threadEntryContext);
	threadEntryContext->exitCode = argument;
	siglongjmp(threadEntryContext->exitJump, 1);
	Errors::unreachable();
}

NO_ASAN static void* forkThreadEntry(void* argsVoid)
{
	std::unique_ptr<ForkThreadArgs> args((ForkThreadArgs*)argsVoid);

	{
		Lock<Platform::Mutex> forkedStackLock(args->forkedStackMutex);
	}

	ThreadEntryContext localThreadEntryContext;
	localThreadEntryContext.framePointer = args->threadEntryFramePointer;
	localThreadEntryContext.exitCode = -1;
	if(!sigsetjmp(localThreadEntryContext.exitJump, 1))
	{
		threadEntryContext = &localThreadEntryContext;
		localThreadEntryContext.exitCode
			= switchToForkedStackContext(&args->forkContext, args->threadEntryFramePointer);
	}

	sigAltStack.deinit();

	return reinterpret_cast<void*>(localThreadEntryContext.exitCode);
}

static void memcpyNoASAN(U8* dest, const U8* source, Uptr numBytes)
{
#if WAVM_ENABLE_ASAN
	bytewiseMemCopy(dest, source, numBytes);

	Uptr shadowShift = 0;
	Uptr shadowOffset = 0;
	__asan_get_shadow_mapping(&shadowShift, &shadowOffset);

	U8* destShadow
		= reinterpret_cast<U8*>((reinterpret_cast<Uptr>(dest) >> shadowShift) + shadowOffset);
	const U8* sourceShadow
		= reinterpret_cast<U8*>((reinterpret_cast<Uptr>(dest) >> shadowShift) + shadowOffset);
	const Uptr shadowNumBytes = numBytes >> shadowShift;

	bytewiseMemCopy(destShadow, sourceShadow, shadowNumBytes);
#else
	memcpy(dest, source, numBytes);
#endif
}

NO_ASAN Thread* Platform::forkCurrentThread()
{
	auto forkThreadArgs = new ForkThreadArgs;

	if(!threadEntryContext)
	{
		Errors::fatal(
			"Cannot fork a thread that wasn't created by Platform::createThread/forkThread");
	}
	if(innermostSignalContext)
	{ Errors::fatal("Cannot fork a thread with catchSignals on the stack"); }

	// Capture the current execution state in forkThreadArgs->forkContext.
	// The forked thread will load this execution context, and "return" from this function on the
	// forked stack.
	const I64 isExecutingInFork = saveExecutionState(&forkThreadArgs->forkContext, 0);
	if(isExecutingInFork)
	{
		// Allocate a sigaltstack for the new thread.
		sigAltStack.init();

		return nullptr;
	}
	else
	{
		forkThreadArgs->forkedStackMutex.lock();

		// Compute the address extent of this thread's stack.
		U8* minStackGuardAddr;
		U8* minStackAddr;
		U8* maxStackAddr;
		getCurrentThreadStack(minStackGuardAddr, minStackAddr, maxStackAddr);
		const Uptr numStackBytes
			= std::max(Uptr(maxStackAddr - minStackAddr), getNumStackBytesRLimit());

		// Use the current stack pointer derive a conservative bounds on the area of the stack that
		// is active.
		const U8* minActiveStackAddr = getStackPointer() - 128;
		const U8* maxActiveStackAddr = threadEntryContext->framePointer;
		const Uptr numActiveStackBytes = maxActiveStackAddr - minActiveStackAddr;

		if(numActiveStackBytes + PTHREAD_STACK_MIN > numStackBytes)
		{ Errors::fatal("not enough stack space to fork thread"); }
		pthread_attr_t threadAttr;
		errorUnless(!pthread_attr_init(&threadAttr));
		errorUnless(!pthread_attr_setstacksize(&threadAttr, numStackBytes));

		auto thread = new Thread;
		errorUnless(!pthread_create(
			&thread->id, &threadAttr, (void* (*)(void*))forkThreadEntry, forkThreadArgs));
		errorUnless(!pthread_attr_destroy(&threadAttr));

		U8* forkedMinStackGuardAddr = nullptr;
		U8* forkedMinStackAddr = nullptr;
		U8* forkedMaxStackAddr = nullptr;
		getThreadStack(thread->id, forkedMinStackGuardAddr, forkedMinStackAddr, forkedMaxStackAddr);

		forkedMaxStackAddr -= PTHREAD_STACK_MIN;
		wavmAssert(Uptr(forkedMaxStackAddr - forkedMinStackAddr) > numActiveStackBytes);

		// Compute the offset to add to stack pointers to translate them to the forked thread's
		// stack.
		const Iptr forkedStackOffset = forkedMaxStackAddr - maxActiveStackAddr;

		// Translate this thread's saved stack pointer to the forked stack.
		forkThreadArgs->forkContext.rsp += forkedStackOffset;

		// Translate this thread's entry stack pointer to the forked stack.
		forkThreadArgs->threadEntryFramePointer
			= threadEntryContext->framePointer + forkedStackOffset;

		// Copy the contents of this thread's stack to the forked stack.
		memcpyNoASAN(
			forkedMaxStackAddr - numActiveStackBytes, minActiveStackAddr, numActiveStackBytes);

		forkThreadArgs->forkedStackMutex.unlock();

		return thread;
	}
}

Uptr Platform::getNumberOfHardwareThreads() { return std::thread::hardware_concurrency(); }

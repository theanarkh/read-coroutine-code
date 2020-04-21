#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;
// 协程调度器
struct schedule {
	// 协程的公共栈
	char stack[STACK_SIZE];
	// 主上下文，不是协程的
	ucontext_t main;
	// 已使用的个数
	int nco;
	// 最大容量
	int cap;
	// 记录哪个协程在执行
	int running;
	// 协程信息
	struct coroutine **co;
};
// 协程的表示
struct coroutine {
	// 协程任务函数
	coroutine_func func;
	// 用户数据，执行func的时候传入
	void *ud;
	// 协程上下文
	ucontext_t ctx;
	// 所属调度器
	struct schedule * sch;
	// 栈最大大小
	ptrdiff_t cap;
	// 栈已用大小
	ptrdiff_t size;
	// 协程状态
	int status;
	// 协程的栈
	char *stack;
};
// 新建一个协程
struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	// 在堆上申请空间
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}
// 释放一个协程的相关内存
void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}
// 创建一个调度器，准备开始执行协程
struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	// 协程数
	S->cap = DEFAULT_COROUTINE;
	// 还没有协程在跑
	S->running = -1;
	// 分申请了一个结构体指针数组，指向协程结构体的信息
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}
// 销毁协程和调度器
void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		// 销毁协程的内存
		if (co) {
			_co_delete(co);
		}
	}
	// 销毁调度器的指针数组
	free(S->co);
	S->co = NULL;
	// 调回调度器本身
	free(S);
}
// 新建一个协程
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	// 申请一个协程结构体
	struct coroutine *co = _co_new(S, func , ud);
	// 协程数达到上限，扩容
	if (S->nco >= S->cap) {
		int id = S->cap;
		// 扩容两倍
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		// 初始化空闲的内存
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		// 有些slot可能是空的，这里从最后一个索引开始找，没找到再从头开始找
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			// 找到可用的slot，记录协程信息
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}
// 封装协程的任务函数
static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	// 协程当前状态
	int status = C->status;
	switch(status) {
    // 可以执行
	case COROUTINE_READY:
		// 保存当前执行的上下文到ctx，在makecontext中会覆盖某些字段
		getcontext(&C->ctx);
		// 设置协程执行时的栈信息，真正的esp在makecontext里会修改成ss_sp+ss_size-一定的大小（用于存储额外数据的）
		C->ctx.uc_stack.ss_sp = S->stack;
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		// 记录下一个协程，即执行完执行他
		C->ctx.uc_link = &S->main;
		// 记录当前执行的协程
		S->running = id;
		// 协程开始执行
		C->status = COROUTINE_RUNNING;
		// 协程执行时的入参
		uintptr_t ptr = (uintptr_t)S;
		// 设置协程（ctx）的任务函数和入参
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		// 保存当前上下文到main，然后切换到ctx对应的上下文执行，即执行上面设置的mainfunc，执行完再切回这里
		swapcontext(&S->main, &C->ctx);
		break;
	// 协程当前是挂起状态，准备变成执行状态
	case COROUTINE_SUSPEND:
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}
// 保存当前栈信息，top是协程的栈顶最大值
static void
_save_stack(struct coroutine *C, char *top) {
	// dummy用于计算出当前的esp，即栈顶地址
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);
	// top-&dummy算出协程当前的栈上下文有多大，如果比当前的容量大，则需要扩容
	if (C->cap < top - &dummy) {
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	// 记录当前的栈大小
	C->size = top - &dummy;
	// 复制公共栈的数据到私有栈
	memcpy(C->stack, &dummy, C->size);
}
// 协程主动让出执行权，切换到main
void
coroutine_yield(struct schedule * S) {
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C,S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	// 当前协程已经让出执行权，当前没有协程执行
	S->running = -1;
	// 切换到main执行
	swapcontext(&C->ctx , &S->main);
}
// 返回协程的状态
int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}
// 返回正在执行的协程
int 
coroutine_running(struct schedule * S) {
	return S->running;
}


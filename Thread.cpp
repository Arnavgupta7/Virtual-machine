#include "Thread.h"

extern "C"{

Thread::Thread()
{
	id = 0;
	priority = 0;
	state = 0;
	stackSize = 0x50000;
	stackBase = NULL;
	waitTimer = 0;
	entry = NULL;
	entryParameters = NULL;
	fileReturn = 0;
	waitingMutex = 0;
}

Thread::~Thread(){
	stackBase = NULL;
	entryParameters = NULL;
}

Thread::Thread(TVMThreadState state, TVMThreadEntry entry, void* param, TVMMemorySize stackSize, TVMThreadPriority priority, TVMThreadID id)
{
	this->state = state;
	this->entry = entry;
	this->stackSize = stackSize;
	this->stackBase = new uint8_t[stackSize];
	this->priority = priority;
	this->id = id;
    entryParameters = param;
}

Thread::Thread(TVMThreadState state, TVMThreadEntry entry, void* param, uint8_t *base, TVMThreadPriority priority, TVMThreadID id)
{
	this->state = state;
	this->entry = entry;
	this->stackSize = stackSize;
	this->stackBase = base;
	this->priority = priority;
	this->id = id;
    entryParameters = param;
}

}
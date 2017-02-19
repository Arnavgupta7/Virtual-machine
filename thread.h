#ifndef THREAD_H
#define THREAD_H

#include "Machine.h"
#include "VirtualMachine.h"
#include <list>
using namespace std;


#ifdef __cplusplus
extern "C" {
#endif

class Thread{
	public:
		TVMThreadID id;
		TVMThreadPriority priority;
		TVMThreadState state;
		SMachineContext context;
		TVMMemorySize stackSize;
		uint8_t *stackBase;
		TVMTick waitTimer;
		TVMThreadEntry entry;
        void *entryParameters;
		int fileReturn;
		TVMMutexID waitingMutex;
		list<TVMMutexID> mutexList;

		Thread(TVMThreadState state, TVMThreadEntry entry, void* param, TVMMemorySize memSize, TVMThreadPriority prio, TVMThreadID id);

		Thread(TVMThreadState state, TVMThreadEntry entry, void* param, uint8_t *base, TVMThreadPriority prio, TVMThreadID id);

		Thread();
		~Thread(); 
};


#ifdef __cplusplus
}
#endif

#endif
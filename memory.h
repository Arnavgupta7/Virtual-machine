#ifndef MEMORY_H
#define MEMORY_H

#include "Machine.h"
#include "VirtualMachine.h"
#include <vector>
using namespace std;


#ifdef __cplusplus
extern "C" {
#endif

struct MemPointer{
	void *base;
	TVMMemorySize length;
};

class Memory
{
	public:
		int PoolID; // This is the pool id
		MemPointer Pool; // This is the mempool base and size
		TVMMemorySize FreeSpace; // This is the amount of total bytes that are free
		vector < MemPointer> FreeVect; // This is a vector of the base/length of each of the free chunks
		vector < MemPointer> AllocVect; // This is a vector (or a map) of the base/length of each of the allocated chunks

		Memory();
		Memory(void *base, TVMMemorySize length, int id);
		~Memory();
};

#ifdef __cplusplus
}
#endif

#endif
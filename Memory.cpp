#include "Memory.h"

extern "C"{

Memory::Memory(){}

Memory::Memory(void* base, TVMMemorySize length, int id)
{
	PoolID = id;
	Pool.base = base;
	Pool.length = length;
	FreeVect.push_back(Pool);
	FreeSpace = length;
}
Memory::~Memory(){}

}
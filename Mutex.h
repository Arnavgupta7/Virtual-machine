#ifndef MUTEX_H
#define MUTEX_H

#include "VirtualMachine.h"
#include <list>
using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

class Mutex{
	public:
		TVMMutexID id;
		TVMThreadID owner;
		list < int > waitHigh;
		list < int > waitMed;
		list < int > waitLow;

		Mutex();
		~Mutex();
};

#ifdef __cplusplus
}
#endif

#endif
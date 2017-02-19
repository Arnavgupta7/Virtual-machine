#include "Mutex.h"

extern "C"{

Mutex::Mutex()
{
	owner = 999;
}
Mutex::~Mutex(){}

}
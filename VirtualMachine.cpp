#include "VirtualMachine.h"
#include "Machine.h"
#include "Thread.h"
#include "Mutex.h"
#include "Memory.h"
#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <list>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <cmath>
#include <bitset>
#include <algorithm>
using namespace std;

extern "C"{

#define IDLE_THREAD_ID 0
#define MAIN_THREAD_ID 1
#define NO_OWNER 999
#define SHARED_POOL_ID 1

///////////////FAT Structs///////////////
struct BPB_STRUCT{
	char* OEMName;
	uint16_t BytsPerSec;
	uint8_t SecPerClus;
	uint16_t RsvdSecCnt;
	uint8_t NumFATs;
	uint16_t RootEntCnt;
	uint16_t TotSec16;
	uint8_t Media;
	uint16_t FATSz16;
	uint16_t SecPerTrk;
	uint16_t NumHeads;
	unsigned int HiddSec;
	unsigned int TotSec32;
	uint8_t DrvNum;
	uint8_t Reserved1;
	uint8_t BootSig;
	char* VolLab;
	char* FilSysType;
};

struct MyDirectoryEntry{
    char DLongFileName[VM_FILE_SYSTEM_MAX_PATH];
    char DShortFileName[VM_FILE_SYSTEM_SFN_SIZE];
    unsigned int DSize;
    unsigned char DAttributes;
    SVMDateTime DCreate;
    SVMDateTime DAccess;
    SVMDateTime DModify;
    uint16_t DIR_FstClusLO;
};

struct CLUST_STRUCT{
	uint8_t* data;
	bool dirty;

	CLUST_STRUCT(): dirty(false){}
};

struct FILE_STRUCT{ // opening new file
	int firstClust;
	int offset;
	int size;
};

struct DIR_STRUCT{
	int startClust;
	int byteOffset;

	DIR_STRUCT();
	DIR_STRUCT(int start);
};
DIR_STRUCT::DIR_STRUCT(){}
DIR_STRUCT::DIR_STRUCT(int start)
{
	startClust = start;
	byteOffset = 0;
}

///////////////Globals///////////////
vector < Thread* > threadPool;
vector < Thread* > sleepPool;
vector < Memory* > memoryPool;
vector < Mutex* > mutexPool;
vector < uint16_t > FATVect;
vector < MyDirectoryEntry* > rootVect;

list < Thread* > lowRQ;
list < Thread* > medRQ;
list < Thread* > highRQ;

map < int, vector < MyDirectoryEntry* > > openDirMap;
map < int, FILE_STRUCT* > openFileMap;
map < int, CLUST_STRUCT > clustMap;

const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;
void *RW_Pool; //global read/write memory pool
BPB_STRUCT BPB;
TVMMutexID FAT_MUTEX = 0;
volatile int FAT_FD;
volatile int currentID;
int directoryDescriptor = 7; // 7 to avoid confusion with other FD
int fileDescriptor = 3; // 3 to avoid stdin and stdout
char currentDir[VM_FILE_SYSTEM_MAX_PATH];
int rootOffset = 0;

//BPB data
volatile uint16_t FirstRootSector;
volatile uint16_t RootDirectorySectors;
volatile uint16_t FirstDataSector;
volatile uint16_t ClusterCount;

//function prototypes
TVMMainEntry VMLoadModule(const char *module);
TVMStatus VMFileSystemGetAbsolutePath(char *abspath, const char *curpath, const char *destpath);

void schedule();
void scheduler();

void PushRQ(Thread *newThread);
void PopRQ(Thread *popThread);
void PushMutexWait(Mutex *mutex, int thread);
void PopMutexWait(Mutex *mutex, int thread);

void Cleanup(int MemoryID);

void Parse_BPB();
void Parse_FAT();
void Parse_ROOT();
void ReadCluster(int clustNum);
char* Parse_String(int offset, int length, char* buff);
char* Parse_FileName(char* buff);
void SeekRead(int startByte, int byteLength, char *buffer);
void SeekReader(int startByte, int byteLength, uint8_t *buffer);
void SeekWrite(int startByte, int byteLength, char *buffer);
int FindCluster(int firstCluster, int clusterSkip);
CLUST_STRUCT GetCluster(int clusterNum);

void SkeletonEntry(void *param)
{
	MachineEnableSignals();
	//get the entry function and param that you need to call
	
	TVMThreadEntry entry = ((Thread*)param)->entry;
	entry(((Thread*)param)->entryParameters);
	VMThreadTerminate(((Thread*)param)->id);
}
void Idle(void *)
{
	while(1);
}

///////////////CallBacks///////////////
void FileCallback(void *calldata, int result)
{	
	((Thread*)calldata)->fileReturn = result;
	((Thread*)calldata)->state = VM_THREAD_STATE_READY;
	PushRQ((Thread*)calldata);
	schedule();
}
void AlarmCallback(void *calldata)
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
	for (unsigned int i = 0; i < sleepPool.size(); i++) 
	{
		sleepPool[i]->waitTimer-- ;
		if (sleepPool[i]->waitTimer == 0)
		{
			sleepPool[i]->state= VM_THREAD_STATE_READY;
			PushRQ(sleepPool[i]);
			
			//delete the element from the sleepPool(now awake)		
			sleepPool.erase(sleepPool.begin() + i);
			schedule();
		}
	}
	MachineResumeSignals(&sigState);
	scheduler();
}

///////////////Start///////////////
TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms,
			      TVMMemorySize sharedsize, const char *mount, int argc, char *argv[])
{
	//init current directory
	currentDir[0] = '/';
	currentDir[1] = '\0';

	void *sharedBase = MachineInitialize(machinetickms, sharedsize);
	MachineEnableSignals();
	void *calldata = NULL;
	MachineRequestAlarm(1000*tickms, AlarmCallback, calldata);

	//create system pool
	uint8_t *Base = new uint8_t[heapsize];
	Memory *systemPool = new Memory(Base, heapsize, VM_MEMORY_POOL_ID_SYSTEM);
	memoryPool.push_back(systemPool);

	//create shared pool
	Memory *sharedPool = new Memory(sharedBase, sharedsize, SHARED_POOL_ID);
	memoryPool.push_back(sharedPool);

	//create main and idle threads
	Thread *idleThread = new Thread(VM_THREAD_STATE_READY, Idle, NULL, (TVMMemorySize)0x50000, 0, IDLE_THREAD_ID);
		threadPool.push_back(idleThread);
		MachineContextCreate(&(threadPool[IDLE_THREAD_ID]->context), 
	 					 SkeletonEntry, (void*)idleThread,
	 					 threadPool[IDLE_THREAD_ID]->stackBase, 
	 					 threadPool[IDLE_THREAD_ID]->stackSize);

	Thread mainThread;
		mainThread.id = threadPool.size();
		mainThread.priority = 2;
		mainThread.state = VM_THREAD_STATE_READY;
		threadPool.push_back(&mainThread);

	currentID = mainThread.id;

    //create FAT mutex
    VMMutexCreate(&FAT_MUTEX);

	//open FAT file
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	void *calldata1 = (void*)threadPool[currentID];
	MachineFileOpen(mount, O_RDWR, 0, FileCallback, calldata1);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();
	FAT_FD = threadPool[currentID]->fileReturn;
	MachineResumeSignals(&sigState);

	//decode BPB
	Parse_BPB();
	//decode FAT
	Parse_FAT();
	//init BPB values
	FirstRootSector = BPB.RsvdSecCnt + BPB.NumFATs * BPB.FATSz16;
	RootDirectorySectors = (BPB.RootEntCnt * 32) / 512;
	FirstDataSector = FirstRootSector + RootDirectorySectors;
	ClusterCount = (BPB.TotSec32 - FirstDataSector) / BPB.SecPerClus;
	
	//decode ROOT
	Parse_ROOT();

	//call main
	TVMMainEntry VMMain = VMLoadModule(argv[0]);
	VMMain(argc, argv);
	
	//clean up
	for(unsigned int i = 0; i < threadPool.size(); i++)
		VMThreadDelete(i);
	for(unsigned int i = 0; i < mutexPool.size(); i++)
		VMMutexDelete(i);

	//dismount fat image
	MachineSuspendSignals(&sigState);

	//write fat
	//write root
	//write clusters

	void *calldata2 = (void*)threadPool[currentID];
	MachineFileClose(FAT_FD, FileCallback, calldata2);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();

	MachineResumeSignals(&sigState);

	//terminate and return
	MachineTerminate();
	if(VMMain == NULL)
		return VM_STATUS_FAILURE;
	else
		return VM_STATUS_SUCCESS;
}

///////////////Thread functions///////////////

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, 
						 TVMThreadPriority prio, TVMThreadIDRef tid)
{
	//suspend signals
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	if(entry == NULL || tid == NULL)
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	//set id
	*tid = ((TVMThreadID)threadPool.size());
	Thread *newThread = new Thread(VM_THREAD_STATE_DEAD, entry, param, memsize, prio, *tid);

    if(VM_STATUS_SUCCESS != VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, 
    											 memsize, 
    											 (void **)newThread->stackBase)){
        VMPrintError("Failed to allocate space\n");
        return VM_STATUS_FAILURE;
    }

	threadPool.push_back(newThread);

	//resume signals
	MachineResumeSignals(&sigState);

	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadDelete(TVMThreadID thread)
{
	if(thread >= threadPool.size())
		return VM_STATUS_ERROR_INVALID_ID;
	if(threadPool[thread]->state != VM_THREAD_STATE_DEAD)
		return VM_STATUS_ERROR_INVALID_STATE;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);\

	delete threadPool[thread];

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadActivate(TVMThreadID thread)
{
	//suspend signals
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	if(thread >= threadPool.size())
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(threadPool[thread]->state != VM_THREAD_STATE_DEAD)
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_STATE;
	}

	//create context
	MachineContextCreate(&(threadPool[thread]->context), 
	 					 SkeletonEntry, (void*)threadPool[thread],
	 					 threadPool[thread]->stackBase, 
	 					 threadPool[thread]->stackSize);
	//set state to ready
	threadPool[thread]->state = VM_THREAD_STATE_READY;

	//add thread to appropriate RQ
	PushRQ(threadPool[thread]);

	schedule();

	//resumesignals
	MachineResumeSignals(&sigState);

	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID thread)
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	if(thread >= threadPool.size())
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	if(threadPool[thread]->state == VM_THREAD_STATE_DEAD)
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_STATE;
	}

	PopRQ(threadPool[thread]);
	threadPool[thread]->state = VM_THREAD_STATE_DEAD;

	schedule();
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref)
{
	if(thread >= threadPool.size())
		return VM_STATUS_ERROR_INVALID_ID;
	if(stateref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	*stateref = threadPool[thread]->state;

	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadSleep(TVMTick tick)
{
	//checking if the tick is infinite
	if(tick == VM_TIMEOUT_INFINITE)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	//suspend signals
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);


	if(tick == VM_TIMEOUT_IMMEDIATE)
		schedule();		
	//change the state of the thread we are concerned with and set waitTimer
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	threadPool[currentID]->waitTimer=tick;

	//push into the sleepPool
	sleepPool.push_back(threadPool[currentID]);

	schedule();

	//resumesignals
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

///////////////Memory Functions///////////////
TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
	if (base == NULL || memory == NULL || size == 0)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//using the variables given we have to contiguously create mempools
	*memory = memoryPool.size();
	Memory *newPool = new Memory(base, size, *memory);
	memoryPool.push_back(newPool);

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
{
	if(memory >= memoryPool.size())
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	if(memoryPool[memory]->FreeSpace != memoryPool[memory]->Pool.length)
		return VM_STATUS_ERROR_INVALID_STATE;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	delete memoryPool[memory];

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef byesleft)
{
	if(memory >= memoryPool.size() || byesleft == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	*byesleft = memoryPool[memory]->FreeSpace;

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
{
	if(memory >= memoryPool.size() || size == 0 || pointer == NULL)
	{
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	if(size % 64 != 0)
	{
		size = size + (64 - size%64);
	}

	unsigned int i;
	bool avail = false;
	for(i = 0; i < memoryPool[memory]->FreeVect.size(); i++)
	{
		MemPointer temp = memoryPool[memory]->FreeVect[i];
		if(size <= temp.length)
		{
			avail = true;
			break;
		}
	}
	if(!avail) 
	{
		return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
	}

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	memoryPool[memory]->FreeSpace -= size;
	memoryPool[memory]->FreeVect[i].length -= size;
	*pointer = memoryPool[memory]->FreeVect[i].base;

	int temp = (int)memoryPool[memory]->FreeVect[i].base;
	temp += size;
	memoryPool[memory]->FreeVect[i].base = (void*)temp;

	MemPointer alloc;
	alloc.length = size;
	alloc.base = *pointer;
	memoryPool[memory]->AllocVect.push_back(alloc);


	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
{
	if(memory >= memoryPool.size() || pointer == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	unsigned int i;
	bool avail = false;
	for(i = 0; i < memoryPool[memory]->AllocVect.size(); i++)
	{
		MemPointer temp = memoryPool[memory]->AllocVect[i];
		if(pointer == temp.base)
		{
			avail = true;
			break;
		}
	}
	if(!avail) return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//modify free space of memoryPool
	memoryPool[memory]->FreeSpace += memoryPool[memory]->AllocVect[i].length;
	//modify free vect based on deallocation
	for(unsigned int j = 0; i < memoryPool[memory]->FreeVect.size(); j++)
	{
		if((int)pointer <= (int)(memoryPool[memory]->FreeVect[j].base))
		{
			MemPointer insert = memoryPool[memory]->AllocVect[i];
			memoryPool[memory]->FreeVect.insert(memoryPool[memory]->FreeVect.begin() + j, insert);

			break;
		}
	}

	//erase allocated block
	memoryPool[memory]->AllocVect.erase(memoryPool[memory]->AllocVect.begin() + i);
	Cleanup(memory);

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}     

///////////////Mutex functions///////////////
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref)
{
	if(mutexref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	Mutex *newMutex = new Mutex();
	*mutexref = mutexPool.size();
	newMutex->id = *mutexref;
	mutexPool.push_back(newMutex);

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex)
{
	if(mutex >= mutexPool.size())
		return VM_STATUS_ERROR_INVALID_ID;
	if(mutexPool[mutex]->owner != NO_OWNER)
		return VM_STATUS_ERROR_INVALID_STATE;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	delete mutexPool[mutex];
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref)
{
	if(mutex >= mutexPool.size())
		return VM_STATUS_ERROR_INVALID_ID;
	if(ownerref == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	TVMThreadID ret = mutexPool[mutex]->owner;
	if(ret != NO_OWNER)
		*ownerref = ret;
	else
		*ownerref = VM_THREAD_ID_INVALID;

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout)
{
	if(mutex >= mutexPool.size())
		return VM_STATUS_ERROR_INVALID_ID;
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
 	//unlocked
	if(mutexPool[mutex]->owner == NO_OWNER)
	 {
		threadPool[currentID]->mutexList.push_back(mutex);
		mutexPool[mutex]->owner = currentID;
	}
	//locked, need to wait
	else if(timeout == VM_TIMEOUT_IMMEDIATE)
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;
	}
	else if(timeout == VM_TIMEOUT_INFINITE)
	{
		PushMutexWait(mutexPool[mutex], currentID);
		//wait
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();
		//acquire mutex
		threadPool[currentID]->mutexList.push_back(mutex);
		mutexPool[mutex]->owner = currentID;
		//remove from wait list
		PopMutexWait(mutexPool[mutex], currentID);
	}
	else
	{
		PushMutexWait(mutexPool[mutex], currentID);
		//wait
		VMThreadSleep(timeout);
		if(mutexPool[mutex]->owner == NO_OWNER)
		{
			//acquire mutex
			threadPool[currentID]->mutexList.push_back(mutex);
			mutexPool[mutex]->owner = currentID;
			//remove from wait list
			PopMutexWait(mutexPool[mutex], currentID);
		}
		else
		{
			MachineResumeSignals(&sigState);
			return VM_STATUS_FAILURE;
		}
	}

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}   

TVMStatus VMMutexRelease(TVMMutexID mutex)
{
	if(mutex >= mutexPool.size())
		return VM_STATUS_ERROR_INVALID_ID;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//remove from thread's held mutexes
	TVMThreadID oldOwner = mutexPool[mutex]->owner;
	threadPool[oldOwner]->mutexList.remove(mutex);

	//unlock mutex
	mutexPool[mutex]->owner = NO_OWNER;


	//choose next highest waiting thread
	if(mutexPool[mutex]->waitHigh.size() != 0)
	{
		//pop off new owner
		TVMThreadID thread = mutexPool[mutex]->waitHigh.front();
		mutexPool[mutex]->waitHigh.pop_front();

		//set new owner to ready
		threadPool[thread]->state = VM_THREAD_STATE_READY;
		PushRQ(threadPool[thread]);
		schedule();
	}
	else if(mutexPool[mutex]->waitMed.size() != 0)
	{


		TVMThreadID thread = mutexPool[mutex]->waitMed.front();
		mutexPool[mutex]->waitMed.pop_front();

		threadPool[thread]->state = VM_THREAD_STATE_READY;
		PushRQ(threadPool[thread]);
		schedule();
	}
	else if(mutexPool[mutex]->waitLow.size() != 0)
	{
		TVMThreadID thread = mutexPool[mutex]->waitLow.front();
		mutexPool[mutex]->waitLow.pop_front();

		//set new owner to ready
		threadPool[thread]->state = VM_THREAD_STATE_READY;
		PushRQ(threadPool[thread]);
		schedule();
	}

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

///////////////File functions///////////////
TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor)
{
	if(filename == NULL || filedescriptor == NULL)
	{
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

    char abspath[VM_FILE_SYSTEM_MAX_PATH];
	VMFileSystemGetAbsolutePath(abspath, (const char*)currentDir, filename);
	char file[VM_FILE_SYSTEM_MAX_PATH];
 	VMFileSystemFileFromFullPath(file, (const char*)abspath);

 	//make file name all caps
 	for(unsigned int i = 0; i < sizeof(file); i++)
 	{
 		if((int)file[i] >= 97 && (int)file[i] <=122)
 		{
 			file[i] = (char)((int)file[i] - 32);
 		}
 	}

 	for(unsigned int i = 0; i < rootVect.size(); i++)
 	{
 		if(strcmp(rootVect[i]->DShortFileName, file) == 0 && rootVect[i]->DAttributes != 0x10)
 		{
 			FILE_STRUCT *openFile = new FILE_STRUCT;
 			openFile->firstClust = rootVect[i] -> DIR_FstClusLO;
 			openFile->offset = 0;
 			openFile->size = rootVect[i] -> DSize;
 			
 			openFileMap.insert(pair<int,FILE_STRUCT*>(fileDescriptor, openFile));
 			*filedescriptor = fileDescriptor;
 			fileDescriptor++;

 			MachineResumeSignals(&sigState);
 	 		return VM_STATUS_SUCCESS; 		
 	 	}
 	}

 	//create new file
 	if(flags == O_CREAT) //might need to change this
 	{	
 		//find first open cluster
 		int i = 0;
 		bool foundOpen = false;
 		for(unsigned int i = 0; i < FATVect.size(); i++)
 		{
 			if(FATVect[i] == 0)
 			{
 				uint16_t x = (uint16_t)0xFFF8;
 				FATVect[i] = x;
 				foundOpen = true;
 				break;
 			} 
 		}
 		if(!foundOpen)
 		{
 			MachineResumeSignals(&sigState);
 	 		return VM_STATUS_FAILURE; 		
 		}

		//create new root vector entry
 		MyDirectoryEntry *tmp = new MyDirectoryEntry;
		strcpy(tmp->DLongFileName, file); 
		strcpy(tmp->DShortFileName, file);
		tmp->DSize = 0;
		tmp->DAttributes = 0;

		SVMDateTimeRef curdatetime = new SVMDateTime;
		VMDateTime(curdatetime);

		tmp->DCreate.DYear = curdatetime->DYear;

		tmp->DCreate.DMonth = curdatetime->DMonth;
		tmp->DCreate.DDay = curdatetime->DDay;
		tmp->DCreate.DHour = curdatetime->DHour;
		tmp->DCreate.DMinute = curdatetime->DMinute;
		tmp->DCreate.DSecond = curdatetime->DSecond;

		tmp->DAccess.DYear = curdatetime->DYear;
		tmp->DAccess.DMonth = curdatetime->DMonth;
		tmp->DAccess.DDay = curdatetime->DDay;
	
		tmp->DModify.DYear = curdatetime->DYear;
		tmp->DModify.DMonth = curdatetime->DMonth;
		tmp->DModify.DDay = curdatetime->DDay;
		tmp->DModify.DHour = curdatetime->DHour;
		tmp->DModify.DMinute = curdatetime->DMinute;
		tmp->DModify.DSecond = curdatetime->DSecond;

		tmp->DIR_FstClusLO = i - 2;


		//modify root?
 		bool foundEmptySlot = false;
		char* Root_Buff = new char[32];
		int start = 512 * FirstRootSector;
 		for (int i = 0; i < BPB.RootEntCnt; ++i) //for each root entry
		{
			SeekRead(start, 32, Root_Buff);

			char name[VM_FILE_SYSTEM_MAX_PATH];
			strcpy(name, Parse_FileName(Root_Buff)); 
			
			//found empty spot
			if(name[0] == 0xE5 || name[0] == 0x00)
			{
				foundEmptySlot = true;

				//fill name
				for(unsigned int i = 0; i < sizeof(tmp->DLongFileName); i++)
				{
					Root_Buff[i] = tmp->DLongFileName[i];
				}

				//attributes
				Root_Buff[11] = tmp->DAttributes;
				//NTRes = 0
				Root_Buff[12] = 0;
				//creation time
				bitset <16> x(((uint16_t)tmp -> DCreate.DDay) << 11);
				bitset <16> y(((uint16_t)tmp -> DCreate.DMonth) << 7);
				bitset <16> z((uint16_t)tmp -> DCreate.DYear);
				uint16_t temp = (((uint16_t)tmp -> DCreate.DDay) << 11) | (((uint16_t)tmp -> DCreate.DMonth) << 7) | ((uint16_t)tmp -> DCreate.DYear);
				Root_Buff[15] = (unsigned char)(temp >> 8);
				Root_Buff[14] = (unsigned char)temp;

				//creation date
				//last access time
				//fst clust hi
				Root_Buff[20] = 0;
				//write time
				//write date
				//fst clust lo
				for(int i = 0; i < 2; i++)
				{
					int x = (tmp->DSize >> (8*i)) & 0xff;
					Root_Buff[26 + i] = x;
				}
				//file size
				for(int i = 0; i < 4; i++)
				{
					int x = (tmp->DSize >> (8*i)) & 0xff;
					Root_Buff[28 + i] = x;
				}
				SeekWrite(start, 32, Root_Buff);
				break;
			}
			start += 32;
		}
		if(foundEmptySlot == false)
		{
 			MachineResumeSignals(&sigState);
 	 		return VM_STATUS_FAILURE; 
		}
 		//if valid, push into root
		rootVect.push_back(tmp);

		//open new file
		FILE_STRUCT *openFile = new FILE_STRUCT;
		openFile->firstClust = rootVect[rootVect.size() - 1] -> DIR_FstClusLO;
		openFile->offset = 0;
		openFile->size = rootVect[rootVect.size() - 1] -> DSize;
 			
		openFileMap.insert(pair<int,FILE_STRUCT*>(fileDescriptor, openFile));
		*filedescriptor = fileDescriptor;
		fileDescriptor++;

		MachineResumeSignals(&sigState);
 		return VM_STATUS_SUCCESS; 		
 	}
 	else
 	{
 		MachineResumeSignals(&sigState);
 	 	return VM_STATUS_FAILURE;
 	}
}

TVMStatus VMFileClose(int filedescriptor)
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	if(filedescriptor >= 3)
	{
		map< int, FILE_STRUCT* >::iterator it;
		it = openFileMap.find(filedescriptor);

		if(it == openFileMap.end()) // file not in map
		{
			MachineResumeSignals(&sigState);
			return VM_STATUS_FAILURE;
		}
		else 
		{
			openFileMap.erase(it); //file found
			MachineResumeSignals(&sigState);
			return VM_STATUS_SUCCESS;
		}
	}
	else
	{
		void *calldata = (void*)threadPool[currentID];
		MachineFileClose(filedescriptor, FileCallback, calldata);
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();
		MachineResumeSignals(&sigState);

		if(threadPool[currentID]->fileReturn >= 0)
			return VM_STATUS_SUCCESS;
		else
			return VM_STATUS_FAILURE;
	}
} 

TVMStatus VMFileRead(int filedescriptor, void *data, int *length)
{

	int localLen = *length;
	int accumulator = 0;

	if(data == NULL || length == NULL)
	{
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	if(filedescriptor >= 3)
	{
		FILE_STRUCT *readFile = openFileMap.at(filedescriptor);

		if(readFile->offset == readFile->size)
		{
			MachineResumeSignals(&sigState);
			return VM_STATUS_FAILURE;
		}

		int clusterSizeInBytes = 512 * BPB.SecPerClus;
		int clusterSkip, clusterOffset, clusterNum;

		if(readFile->offset + (*length) >= readFile->size)
		{
			localLen = readFile->size - readFile->offset;
		}

		while(localLen > 0)
		{
			clusterSkip = readFile->offset / clusterSizeInBytes;
			clusterOffset = readFile->offset % clusterSizeInBytes;
			clusterNum = FindCluster(readFile -> firstClust, clusterSkip); 

			int readBytes;
			if(localLen < clusterSizeInBytes - clusterOffset)
			{
				readBytes = localLen;
			}
			else readBytes = clusterSizeInBytes - clusterOffset;

			if (clusterNum == -1)
			{
				MachineResumeSignals(&sigState);
				return VM_STATUS_FAILURE;
			}

			CLUST_STRUCT newCluster = GetCluster(clusterNum);

			memcpy(data, newCluster.data + clusterOffset, readBytes);

			data = (uint8_t*)data + readBytes;
			localLen -= readBytes;
			accumulator += readBytes;
			readFile->offset += readBytes;
		}

		*length = accumulator;

		MachineResumeSignals(&sigState);
		return VM_STATUS_SUCCESS;
	}
	else
	{
		//allocate block in shared memory to read to
		void *pointer;
	    if(VM_STATUS_SUCCESS != VMMemoryPoolAllocate(SHARED_POOL_ID, 
	    						*length >= 512 ? 512 : *length, 
	    						&pointer)){
	        VMPrintError("Failed to allocate space\n");
			MachineResumeSignals(&sigState);
	        return VM_STATUS_FAILURE;
	    }
		//read 512 blocks
		while(localLen > 512)
		{
			void *calldata = (void*)threadPool[currentID];
			MachineFileRead(filedescriptor, pointer, 512, FileCallback, calldata);
			threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
			schedule();
			accumulator += threadPool[currentID]->fileReturn;
			memcpy(data, pointer, 512);
			data = (void*)((int*)data + 512);
			localLen -= 512;
		}
		//check for remaining bytes
		if(localLen != 0)
		{
			void *calldata = (void*)threadPool[currentID];
			MachineFileRead(filedescriptor, pointer, localLen, FileCallback, calldata);
			threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
			schedule();	
			accumulator += threadPool[currentID]->fileReturn;
			memcpy(data, pointer, localLen);
		}

		//deallocate memory
	    if(VM_STATUS_SUCCESS != VMMemoryPoolDeallocate(SHARED_POOL_ID, pointer)){
	        VMPrintError("Failed to deallocate space\n");
	            return VM_STATUS_FAILURE;
	    }

	    //set length
	    *length = accumulator;
		MachineResumeSignals(&sigState);
		if(threadPool[currentID]->fileReturn >= 0)
			return VM_STATUS_SUCCESS;
		else
			return VM_STATUS_FAILURE;
	}
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length)
{
	int localLen = *length;
	int accumulator = 0;

	if(data == NULL || length == NULL)
	{
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//allocate block in shared pool to write to
	void *pointer;
    if(VM_STATUS_SUCCESS != VMMemoryPoolAllocate(SHARED_POOL_ID, 
    						*length >= 512 ? 512 : *length, 
    						&pointer)){
        VMPrintError("Failed to allocate space\n");
        return VM_STATUS_FAILURE;
    }
    //write 512 blocks
	while(localLen > 512)
	{
		memcpy(pointer, data, 512);
		void *calldata = (void*)threadPool[currentID];
		MachineFileWrite(filedescriptor, pointer, 512, FileCallback, calldata);
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();	
		accumulator += threadPool[currentID]->fileReturn;

		data = (void*)((int*)data + 512);
		localLen -= 512;
	}
	//check for remaining bytes
	if(localLen != 0)
	{
		memcpy(pointer, data, localLen);
		void *calldata = (void*)threadPool[currentID];
		MachineFileWrite(filedescriptor, pointer, localLen, FileCallback, calldata);
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();	
		accumulator += threadPool[currentID]->fileReturn;

	}

	//deallocate memory
    if(VM_STATUS_SUCCESS != VMMemoryPoolDeallocate(SHARED_POOL_ID, pointer)){
        VMPrintError("Failed to deallocate space\n");
            return VM_STATUS_FAILURE;
    }

    //set length
    *length = accumulator;
	MachineResumeSignals(&sigState);

	if(threadPool[currentID]->fileReturn >= 0)
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;
}


// TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
// {
// 	TMachineSignalState sigState;
// 	MachineSuspendSignals(&sigState);

// 	void *calldata = (void*)threadPool[currentID];
// 	MachineFileSeek(filedescriptor, offset, whence, FileCallback, calldata);
// 	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
// 	schedule();	
// 	*newoffset = threadPool[currentID]->fileReturn;
// 	MachineResumeSignals(&sigState);
// 	if(threadPool[currentID]->fileReturn >= 0)
// 		return VM_STATUS_SUCCESS;
// 	else
// 		return VM_STATUS_FAILURE;
// }

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset)
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
	
	if(filedescriptor >= 3)
	{

		map< int, FILE_STRUCT* >::iterator it;
		it = openFileMap.find(filedescriptor);

		if(it == openFileMap.end()) // file not in map
		{
			MachineResumeSignals(&sigState);
			return VM_STATUS_FAILURE;
		}
		else 
		{
			*newoffset = whence + offset;
			it->second->offset = *newoffset;
			MachineResumeSignals(&sigState);
			return VM_STATUS_SUCCESS;
		}
	}	
	
	else
	{
		void *calldata = (void*)threadPool[currentID];
		MachineFileSeek(filedescriptor, offset, whence, FileCallback, calldata);
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();	
		*newoffset = threadPool[currentID]->fileReturn;
		MachineResumeSignals(&sigState);
		if(threadPool[currentID]->fileReturn >= 0)
			return VM_STATUS_SUCCESS;
		else
			return VM_STATUS_FAILURE;
	}
}

///////////////Directory functions///////////////

TVMStatus VMDirectoryOpen(const char *dirname, int *dirdescriptor)
{

	if(dirname == NULL || dirdescriptor == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//if root
	const char* root = "/";
	if(strcmp(dirname, root) == 0)
	{
		*dirdescriptor = directoryDescriptor;
		openDirMap.insert( pair<int, vector < MyDirectoryEntry* > >(directoryDescriptor, rootVect) );
		directoryDescriptor++;

		MachineResumeSignals(&sigState);
		return VM_STATUS_SUCCESS;
	}
	else
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;
	}	
}

TVMStatus VMDirectoryClose(int dirdescriptor)
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	map< int, vector < MyDirectoryEntry* > >::iterator it;
	it = openDirMap.find(dirdescriptor);

	if(it == openDirMap.end()) // directory not in map
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;
	}
	else openDirMap.erase(it); //directory found
	VMDirectoryRewind(1);

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent)
{
	if (dirent == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//at end of directory
	if(openDirMap.at(dirdescriptor).size() == (unsigned)rootOffset)
	{
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;
	}

	SVMDirectoryEntry ret;
	// MyDirectoryEntry* dir = openDirMap.at(dirdescriptor)[rootOffset];

	strcpy(ret.DLongFileName,openDirMap.at(dirdescriptor)[rootOffset]->DLongFileName);
	strcpy(ret.DShortFileName,openDirMap.at(dirdescriptor)[rootOffset]->DShortFileName);
	ret.DSize = openDirMap.at(dirdescriptor)[rootOffset]->DSize;

	ret.DAttributes = openDirMap.at(dirdescriptor)[rootOffset]->DAttributes;
	ret.DCreate = openDirMap.at(dirdescriptor)[rootOffset]->DCreate;
	ret.DAccess = openDirMap.at(dirdescriptor)[rootOffset]->DAccess;
	ret.DModify = openDirMap.at(dirdescriptor)[rootOffset]->DModify;

	rootOffset++;

	*dirent = ret; // return directory entry

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}
TVMStatus VMDirectoryRewind(int dirdescriptor)
{
	rootOffset = 0;

	return VM_STATUS_SUCCESS;
}
TVMStatus VMDirectoryCurrent(char *abspath)
{
	if(abspath == NULL)
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

    const char* path = (const char*)currentDir;
	VMFileSystemGetAbsolutePath(abspath, path, path);

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}
TVMStatus VMDirectoryChange(const char *path)
{
    char abspath[VM_FILE_SYSTEM_MAX_PATH];
	VMFileSystemGetAbsolutePath(abspath, (const char*)currentDir, path);

	if(strcmp(abspath, ".") == 0 || strcmp(abspath, "./") == 0 || strcmp(abspath, "/") == 0)
	{
		return VM_STATUS_SUCCESS;
	}
	else return VM_STATUS_FAILURE;
}

///////////////Helper functions///////////////

void Cleanup(int MemoryID)
{
	unsigned int i=0;
	void *previousBase = NULL;
	TVMMemorySize previousLength = 0;

	for (i = 1; i < memoryPool[MemoryID]->FreeVect.size(); ++i)
	{
			previousBase = memoryPool[MemoryID]->FreeVect[i-1].base;
			previousLength = memoryPool[MemoryID]->FreeVect[i-1].length;
			previousBase = (void*)((int)previousBase + previousLength);

			//checks for pools that are contiguous and makes them one
			if(*(int*)(memoryPool[MemoryID]->FreeVect[i].base) == *(int*)previousBase)
			{
				memoryPool[MemoryID]->FreeVect[i-1].length += memoryPool[MemoryID]->FreeVect[i].length;
				memoryPool[MemoryID]->FreeVect.erase(memoryPool[MemoryID]->FreeVect.begin() + i);
				--i;
			}
	}
}

void schedule()
{
	if(highRQ.size() != 0)
	{	
		if(highRQ.front()->priority > threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)
		{
			//get contexts
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(highRQ.front()->context);

			//set new thread to running
			highRQ.front()->state = VM_THREAD_STATE_RUNNING;

			//set old thread to ready and push into RQ if current thread is lower prio
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				PushRQ(threadPool[currentID]);
			}

			//change current ID and remove new thread from RQ
			currentID = highRQ.front()->id;
			highRQ.pop_front();			
			//switch context
			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(medRQ.size() != 0)
    {			
		if(medRQ.front()->priority > threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)		
		{
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(medRQ.front()->context);

			medRQ.front()->state = VM_THREAD_STATE_RUNNING;
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				if(currentID != IDLE_THREAD_ID)
				PushRQ(threadPool[currentID]);
			}

			currentID = medRQ.front()->id;
			medRQ.pop_front();
			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(lowRQ.size() != 0)
	{	
		if(lowRQ.front()->priority > threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)
		{
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(lowRQ.front()->context);

			lowRQ.front()->state = VM_THREAD_STATE_RUNNING;
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				PushRQ(threadPool[currentID]);
			}

			currentID = lowRQ.front()->id;
			lowRQ.pop_front();

			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(currentID != IDLE_THREAD_ID) //switch to idle thread
	{	
		volatile int oldID = currentID;

		currentID = IDLE_THREAD_ID;
		MachineContextSwitch(&(threadPool[oldID]->context), 
						     &(threadPool[currentID]->context));
	}
}
void scheduler()
{
	if(highRQ.size() != 0)
	{	
		if(highRQ.front()->priority >= threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)
		{
			//get contexts
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(highRQ.front()->context);

			//set new thread to running
			highRQ.front()->state = VM_THREAD_STATE_RUNNING;

			//set old thread to ready and push into RQ if current thread is lower prio
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				PushRQ(threadPool[currentID]);
			}

			//change current ID and remove new thread from RQ
			currentID = highRQ.front()->id;
			highRQ.pop_front();			
			//switch context
			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(medRQ.size() != 0)
    {			
		if(medRQ.front()->priority >= threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)		
		{
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(medRQ.front()->context);

			medRQ.front()->state = VM_THREAD_STATE_RUNNING;
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				if(currentID != IDLE_THREAD_ID)
				PushRQ(threadPool[currentID]);
			}

			currentID = medRQ.front()->id;
			medRQ.pop_front();
			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(lowRQ.size() != 0)
	{	
		if(lowRQ.front()->priority >= threadPool[currentID]->priority ||
			threadPool[currentID]->state == VM_THREAD_STATE_WAITING)
		{
			SMachineContextRef oldContext = &(threadPool[currentID]->context);
			SMachineContextRef newContext = &(lowRQ.front()->context);

			lowRQ.front()->state = VM_THREAD_STATE_RUNNING;
			if(threadPool[currentID]->state == VM_THREAD_STATE_RUNNING)
			{
				threadPool[currentID]->state = VM_THREAD_STATE_READY;
				PushRQ(threadPool[currentID]);
			}

			currentID = lowRQ.front()->id;
			lowRQ.pop_front();

			MachineContextSwitch(oldContext, newContext);
		}
	}
	else if(currentID != IDLE_THREAD_ID) //switch to idle thread
	{	
		volatile int oldID = currentID;

		currentID = IDLE_THREAD_ID;
		MachineContextSwitch(&(threadPool[oldID]->context), 
						     &(threadPool[currentID]->context));
	}
}

void PushRQ(Thread *newThread)
{
	int prio = newThread->priority;
	switch(prio)
	{
		case 1:
			lowRQ.push_back(newThread); break;
		case 2:
			medRQ.push_back(newThread); break;
		case 3:
			highRQ.push_back(newThread); break;
		default:
			break;
	}	
}

void PopRQ(Thread *popThread)
{
	int prio = popThread->priority;
	switch(prio)
	{
		case 1:
			lowRQ.remove(popThread); break;
		case 2:
			medRQ.remove(popThread); break;
		case 3:
			highRQ.remove(popThread); break;
		default:
			break;
	}
}
void PushMutexWait(Mutex *mutex, int thread)
{
	int prio = threadPool[thread]->priority;
	switch(prio)
	{
		case 1:
			mutex->waitLow.push_back(thread);
			break;
		case 2:
			mutex->waitMed.push_back(thread);
			break;
		case 3:
			mutex->waitHigh.push_back(thread);
			break;
	}
}

void PopMutexWait(Mutex *mutex, int thread)
{
	int prio = threadPool[thread]->priority;
	switch(prio)
	{
		case 1:
			mutex->waitLow.remove(thread);
			break;
		case 2:
			mutex->waitMed.remove(thread);
			break;
		case 3:
			mutex->waitHigh.remove(thread);
			break;
	}
}

void Parse_BPB()
{
	//read FAT file
	char* BPB_Buff = new char[512];

	SeekRead(0, 512, BPB_Buff);

	BPB.OEMName = Parse_String(3, 8, BPB_Buff);
	BPB.BytsPerSec = BPB_Buff[11] + (((uint16_t)BPB_Buff[12])<<8);
	BPB.SecPerClus = BPB_Buff[13];
	BPB.RsvdSecCnt = BPB_Buff[14] + (((uint16_t)BPB_Buff[15])<<8);
	BPB.NumFATs = BPB_Buff[16];
	BPB.RootEntCnt = BPB_Buff[17] + (((uint16_t)BPB_Buff[18])<<8);
	BPB.TotSec16 = BPB_Buff[19] + (((uint16_t)BPB_Buff[20])<<8);
	BPB.Media = BPB_Buff[21];
	BPB.FATSz16 = BPB_Buff[22] + (((uint16_t)BPB_Buff[23])<<8);
	BPB.SecPerTrk = BPB_Buff[24] + (((uint16_t)BPB_Buff[25])<<8);
	BPB.NumHeads = BPB_Buff[26] + (((uint16_t)BPB_Buff[27])<<8);
	BPB.HiddSec = BPB_Buff[28] + (((uint16_t)BPB_Buff[29])<<8) + (((uint16_t)BPB_Buff[30])<<16) + (((uint16_t)BPB_Buff[31])<<32);
	BPB.TotSec32 = BPB_Buff[32] + (((uint16_t)BPB_Buff[33])<<8) + (((uint16_t)BPB_Buff[34])<<16) + (((uint16_t)BPB_Buff[35])<<32);
	BPB.DrvNum = BPB_Buff[36];
	BPB.Reserved1 = 0; //default value
	BPB.BootSig = BPB_Buff[38]; //default value	
	BPB.VolLab = Parse_String(43, 11, BPB_Buff);
	BPB.FilSysType = Parse_String(54, 8, BPB_Buff);
}

void Parse_FAT()
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	for(int j = 0; j < BPB.FATSz16; j++) // for each sector in the FAT
	{
		void *calldata = (void*)threadPool[currentID];
		MachineFileRead(FAT_FD, RW_Pool, 512, FileCallback, calldata);
		threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
		schedule();
		uint16_t *FATBuff = new uint16_t[512];
		FATBuff = (uint16_t *)RW_Pool;

		for(int k = 0; k < 256; k++) // for each 2 byte entry
		{
			FATVect.push_back(FATBuff[k]); 
		}	
	}

	MachineResumeSignals(&sigState);
}

void Parse_ROOT()
{
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
	char* Root_Buff = new char[32];
	int start = 512 * FirstRootSector;

	for (int i = 0; i < BPB.RootEntCnt; ++i) //for each root entry
	{
		SeekRead(start, 32, Root_Buff);
		start += 32;

		MyDirectoryEntry *tmp = new MyDirectoryEntry;

		//Names
		strcpy(tmp->DLongFileName, Parse_FileName(Root_Buff));
		strcpy(tmp->DShortFileName, tmp->DLongFileName);
		//Size
		tmp->DSize = Root_Buff[28] + (((uint16_t)Root_Buff[29])<<8) + (((uint16_t)Root_Buff[30])<<16) + (((uint16_t)Root_Buff[31])<<32);
		//Attribute
		tmp->DAttributes = Root_Buff[11];

		//Create Date
		uint16_t byte2 = ((uint16_t)Root_Buff[17]) << 8;
		unsigned char byte1 = Root_Buff[16];
		uint16_t concat = byte2 | byte1;
		//bits 9-15
		tmp->DCreate.DYear = 1980 + ((concat & 65024) >> 9);
		//bits 5-8
		tmp->DCreate.DMonth = (concat & 480) >> 5;
		//bits 0-4
		tmp->DCreate.DDay = (concat & 31);

		//Create Time
		byte2 = ((uint16_t)Root_Buff[15]) << 8;
		byte1 = Root_Buff[14];
		concat = byte2 | byte1;		
		//bits 11-15
		tmp->DCreate.DHour = (concat & 63488) >> 11;
		//bits 5-10
		tmp->DCreate.DMinute = (concat & 2016) >> 5;
		//bits 0-4
		tmp->DCreate.DSecond = 2* (concat & 31);

		//Access Date
		byte2 = ((uint16_t)Root_Buff[19]) << 8;
		byte1 = Root_Buff[18];
		concat = byte2 | byte1;
		tmp->DAccess.DYear = 1980 + ((concat & 65024) >> 9);
		tmp->DAccess.DMonth = (concat & 480) >> 5;
		tmp->DAccess.DDay = (concat & 31);

		//modify date and time
		byte2 = ((uint16_t)Root_Buff[25]) << 8;
		byte1 = Root_Buff[24];
		concat = byte2 | byte1;	
		tmp->DModify.DYear = 1980 + ((concat & 65024) >> 9);
		tmp->DModify.DMonth = (concat & 480) >> 5;
		tmp->DModify.DDay = (concat & 31);

		byte2 = ((uint16_t)Root_Buff[23]) << 8;
		byte1 = Root_Buff[22];
		concat = byte2 | byte1;
		tmp->DModify.DHour = (concat & 63488) >> 11;
		tmp->DModify.DMinute = (concat & 2016) >> 5;
		tmp->DModify.DSecond = 2 * (concat & 31);

		tmp->DIR_FstClusLO = Root_Buff[26] + (((uint16_t)Root_Buff[27])<<8);

		if(tmp->DLongFileName[0] != 0xE5 &&  tmp->DLongFileName[0] != 0x00 
		   && tmp->DAttributes != (0x01 | 0x02 | 0x04 | 0x08))// not equal to long name entry
		{

			rootVect.push_back(tmp);
		} 
	}
	MachineResumeSignals(&sigState);
}

char* Parse_String(int offset, int length, char* buff)
{
	//turn byte sequence into char*

	char* ret = new char[length + 1];
	for(int i = 0 ; i < length; i++)
			ret[i] = (char)buff[i + offset];

	ret[length] = '\0';

	return ret;
}

char* Parse_FileName(char* buff)
{
	char* ret = new char[13];
	int index = 0;
	bool period = true;
	for(int i = 0; i < 11; i++)
	{
		if(i < 8 && buff[i] != ' ')
		{
			ret[index] = (char)buff[i];
			index++;
		}
		else if(i  >= 8 && buff[i] != ' ')
		{
			if(period)
			{
				ret[index] = '.';
				index++;
				period = false;
			}
			ret[index] = (char)buff[i];
			index++;
		}
	}
	ret[index] = '\0';

	return ret;
}

void SeekRead(int startByte, int byteLength, char *buffer)
{
	VMMutexAcquire(FAT_MUTEX, VM_TIMEOUT_INFINITE);

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//allocate memory for reads and writes
	VMMemoryPoolAllocate(SHARED_POOL_ID, 512, &RW_Pool);

	//seek in file
	void *calldata = (void*)threadPool[currentID];
	MachineFileSeek(FAT_FD, startByte, 0, FileCallback, calldata);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	

	//read sector
	void *calldata1 = (void*)threadPool[currentID];
	MachineFileRead(FAT_FD, RW_Pool, byteLength, FileCallback, calldata1);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	

	//fill buffer
	memcpy(buffer, RW_Pool, byteLength);

	VMMemoryPoolDeallocate(SHARED_POOL_ID, RW_Pool);

	MachineResumeSignals(&sigState);

	VMMutexRelease(FAT_MUTEX);
}

void SeekReader(int startByte, int byteLength, uint8_t *buffer)
{
	VMMutexAcquire(FAT_MUTEX, VM_TIMEOUT_INFINITE);

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//allocate memory for reads and writes
	VMMemoryPoolAllocate(SHARED_POOL_ID, 512, &RW_Pool);

	//seek in file
	void *calldata = (void*)threadPool[currentID];
	MachineFileSeek(FAT_FD, startByte, 0, FileCallback, calldata);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	

	//read sector
	void *calldata1 = (void*)threadPool[currentID];
	MachineFileRead(FAT_FD, RW_Pool, byteLength, FileCallback, calldata1);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	

	//fill buffer
	memcpy(buffer, RW_Pool, byteLength);

	VMMemoryPoolDeallocate(SHARED_POOL_ID, RW_Pool);

	MachineResumeSignals(&sigState);

	VMMutexRelease(FAT_MUTEX);
}

void SeekWrite(int startByte, int byteLength, char *buffer)
{

	VMMutexAcquire(FAT_MUTEX, VM_TIMEOUT_INFINITE);

	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);

	//allocate memory for reads and writes
    VMMemoryPoolAllocate(SHARED_POOL_ID, 512, &RW_Pool);

	//seek in file
	void *calldata = (void*)threadPool[currentID];
	MachineFileSeek(FAT_FD, startByte, 0, FileCallback, calldata);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	
	//fill buffer
	memcpy(RW_Pool, buffer, byteLength);
	void *calldata1 = (void*)threadPool[currentID];
	MachineFileWrite(FAT_FD, RW_Pool, byteLength, FileCallback, calldata1);
	threadPool[currentID]->state = VM_THREAD_STATE_WAITING;
	schedule();	
	VMMemoryPoolDeallocate(SHARED_POOL_ID, RW_Pool);
	MachineResumeSignals(&sigState);
	VMMutexRelease(FAT_MUTEX);

}

int FindCluster(int firstCluster, int clusterSkip)
{

	if (clusterSkip == 0) return firstCluster;

	int retClust = firstCluster;
	for(int i = 0; i < clusterSkip; i++)
	{
		if(retClust >= 0xFFF8)
		{
			return -1;
		}	

		retClust = FATVect[retClust];
	}
	return retClust;
}

CLUST_STRUCT GetCluster(int clusterNum)
{
	//get cluster in cache or create new cluster

	map< int, CLUST_STRUCT >::iterator it;
	it = clustMap.find(clusterNum);

	if(it == clustMap.end()) // directory not in map
	{
		//create new struct
		CLUST_STRUCT newCluster;
		newCluster.data = new uint8_t[BPB.SecPerClus * BPB.BytsPerSec];
		
		int num = (FirstDataSector + ((clusterNum-2) * BPB.SecPerClus)) * BPB.BytsPerSec;

		SeekReader(num, BPB.BytsPerSec, newCluster.data); 
		num+=512;
		SeekReader(num, BPB.BytsPerSec, newCluster.data + 512); 

		clustMap.insert(pair<int, CLUST_STRUCT>(clusterNum, newCluster));
		return newCluster;
	}
	else //directory found
	{
		return clustMap.at(clusterNum);
	}
}

}//end of extern C
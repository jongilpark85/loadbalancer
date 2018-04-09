#include "CLoadBalancer.h"

// Thread Argument
struct ThreadData
{
	unsigned short usPortForClient; // Port number open to clients
	unsigned short usPortForServer; // Port number open to servers
	int iThreadIndex; // Each thread's index used to perform write operations on its own area of global data
};

// Use the values provided as command line arguments if any
// load balancer port for clients, load balancer port for servers 
int ParseArguments(int argc, char* argv[], unsigned short* pLBPortForClient_, unsigned short* pLBPortForServer_);

// This is the function invoked on creation of a thread (pthread_create)
void *ThreadMain(void *pArg_);

// Main Function
int main(int argc, char *argv[])
{
	unsigned short usPortForClient = LB_PORT_FOR_CLIENT;
	unsigned short usPortForServer = LB_PORT_FOR_SERVER;
	
	// Use the values provided as command line arguments if any
	if (-1 == ParseArguments(argc, argv, &usPortForClient, &usPortForServer))
		exit(EXIT_FAILURE);
	

	// Create Threads
	pthread_t uiThread[MAX_THREAD_COUNTS];
	ThreadData stThreadInfo[MAX_THREAD_COUNTS];
	
	for (int i = 0; i < MAX_THREAD_COUNTS -1; ++i)
	{
		stThreadInfo[i].usPortForClient = usPortForClient;
		stThreadInfo[i].usPortForServer = usPortForServer;
		stThreadInfo[i].iThreadIndex = i;
	
		pthread_create(&uiThread[i], NULL, &ThreadMain, (void*)&stThreadInfo[i]);
	}
	
	// Run the load balancer in the main thread as well
	const int iLastIndex = MAX_THREAD_COUNTS - 1;
	stThreadInfo[iLastIndex].usPortForClient = usPortForClient;
	stThreadInfo[iLastIndex].usPortForServer = usPortForServer;
	stThreadInfo[iLastIndex].iThreadIndex = iLastIndex;
	ThreadMain((void*)&stThreadInfo[iLastIndex]);

	return 0;
}

// This is the function invoked on creation of a thread (pthread_create)
void *ThreadMain(void *pArg_)
{
	struct ThreadData* pData = (ThreadData*)pArg_;
	
	// Create an Load Balancer Instance
	CLoadBalancer* pLoadBalancer =  new CLoadBalancer(pData->usPortForClient, pData->usPortForServer, pData->iThreadIndex);
	
	//  Set Up the Load Balancer
	if (-1 == pLoadBalancer->SetUp())
	{
		pLoadBalancer->DisplayErrorMessage("SetUp() Failed");
		return NULL;
	}
	
	// Run the Load Balancer
	pLoadBalancer->Run();
	
	return NULL;
}

// Use the values provided as command line arguments if any
// load balancer port for clients, load balancer port for servers 
// Return -1 on Failure
// Return 0 on Success
int ParseArguments(int argc, char* argv[], unsigned short* pLBPortForClient_, unsigned short* pLBPortForServer_)
{
	if (2 <= argc)
	{
		int iLBPortForClient = atoi(argv[1]);
		if (iLBPortForClient < 0 || 65535 < iLBPortForClient)
		{
			printf("Load balancer Invalid Port Number for client\n");
			return -1;
		}
		
		*pLBPortForClient_ = (unsigned short)iLBPortForClient;
	}
	
	if (3 <= argc)
	{
		int iLBPortForServer = atoi(argv[2]);
		if (iLBPortForServer < 0 || 65535 < iLBPortForServer)
		{
			printf("Load balancer Invalid Port Number for server\n");
			return -1;
		}
		
		*pLBPortForServer_ = (unsigned short)iLBPortForServer;
	}
	
	return 0;
}

#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include "Common_Header.h"

// Thread arguments
struct ThreadData
{
	int iEpollFD;
	int iListenSockFD;
};

ThreadData g_stArg;

// Default port number open to clients
#define DEFAULT_PORT_FOR_CLIENT 47000

// For easy test on local machine
// Default IP of the load balancer
#define DEFAULT_LB_IP "127.0.0.1"

// The server sends its status to the load balancer every UPDATE_TIME_INTERVAL microseconds
#define UPDATE_TIME_INTERVAL 1

// Up to MAX_EVENT_COUNTS are returned by epoll_wait()
#define MAX_EVENT_COUNTS 256

// Same reasoning as UDP packet receive
// The maximum number of client connections the load balancer accepts in a loop
#define MAX_CLIENT_ACCEPT_LOOPING_COUNT		SOMAXCONN

// The number of clients currently connected to the server
long int g_iClientCounts = 0;

// Get Command Line Arguments if provided
// Server port, load balancer IP, load balancer port 
int ParseArguments(int argc, char* argv[], unsigned short* pServerPort_, struct in_addr* pLB_IP_, unsigned short* pLBPort_);

// Set up epoll and lisening socket to accept incoming connections or packets
// Run the server in another thread
int SetUpServer(unsigned short usServerPort_);

// Create and set up a TCP listening socket for accepting incoming connection from clients
int SetUpListenSock(int iEpollFD_, unsigned short usPort_);

// Set up socket options
int SetSocketOptions(int iSockFD_);

// Communicate with clients in another thread
void* ThreadMain(void* pArg_);

// Handle All Epoll events
int EpollEventHandler(int iEpollFD_, int iListenSockFD_, int iEventFD_, uint32_t uiEpollEvent_);

// Handle a disconnected client
int DisconnectHandler(int iEpollFD_, int iSockFD_);

// Handle EPOLLIN event
int EpollInHandler(int iEpollFD_, int iListenSockFD_, int iEventFD_);

// Receive a packet from a client
int RecvClientPacket(int iSockFD_);

// Accept client connectons
int AcceptConnection(int iEpollFD_, int iListenSockFD_);

// Initiate Connection with the load balancer
int ConnectToLoadBalancer(in_addr_t uiIP_, unsigned short usPort_);

// Send the Load Balancer the port number on which the server is listening 
int SendServerPort(int iLBSockFD_, unsigned short usServerPort_);

// Send the load balancer the number of clients currently connected to the server
// The number of connected clients repsents how busy the server is 
int SendServerStatus(int iLBSockFD_);

// Handling SIGPIPE signal (For testing)
void SignalHandler(int iSignal_);

// Print out the number of clients currently connected (For testing)
void OnExit();

// Main Function
int main(int argc, char *argv[])
{
	// For testing
	signal(SIGPIPE, SignalHandler);
	//atexit(OnExit);
	
	unsigned short usServerPort = DEFAULT_PORT_FOR_CLIENT; // Port on which clients connect to the servver
	unsigned short usLBPort = LB_PORT_FOR_SERVER; // Load Balancer Port
	struct in_addr stLB_IP; // Load Balancer IP

	// Get Command Line Arguments
	if (-1 == ParseArguments(argc, argv, &usServerPort, &stLB_IP, &usLBPort))
		exit(EXIT_FAILURE);
	
	// Set up the Server and run it in another thread
	if (-1 == SetUpServer(usServerPort))
		exit(EXIT_FAILURE);

	// Initiate Connection to the load balancer
	int iLBSockFD = ConnectToLoadBalancer(stLB_IP.s_addr, usLBPort);
	if (-1 == iLBSockFD)
		exit(EXIT_FAILURE);
		
	// Send the Load Balancer the port number on which the server is listening 
	if (-1 == SendServerPort(iLBSockFD, usServerPort))
		exit(EXIT_FAILURE);

	// Repeatedly send status information to the load balaner on a regular time basis
	while(1)
	{
		if (-1 == SendServerStatus(iLBSockFD))
			exit(EXIT_FAILURE);
		
		sleep(UPDATE_TIME_INTERVAL);
	}

	return 0;
}

// Get Command Line Arguments if provided
// Server port, load balancer IP, load balancer port 
// Return -1 on Failure
// Return 0 on Success
int ParseArguments(int argc, char* argv[], unsigned short* pServerPort_, struct in_addr* pLB_IP_, unsigned short* pLBPort_)
{
	if (2 <= argc)
	{
		int iServerPort = atoi(argv[1]);
		if (iServerPort < 0 || 65535 < iServerPort)
		{
			printf("Server Invalid Port Number\n");
			return -1;
		}
		
		*pServerPort_ = (unsigned short)iServerPort;
	}
		
	
	if (3 <= argc)
	{
		if (0 == inet_aton(argv[2], pLB_IP_))
		{
			printf("inet_aton() Address provided is invalid\n");
			return -1;
		}
	}
	else
	{
		if (0 == inet_aton(DEFAULT_LB_IP, pLB_IP_))
		{
			printf("inet_aton() Default address is invalid\n");
			return -1;
		}
	}
		
	
	if (4 <= argc)
	{
		int iLBPort = atoi(argv[3]);
		if (iLBPort < 0 || 65535 < iLBPort)
		{
			printf("Load balancer Invalid Port Number\n");
			return -1;
		}
		
		*pLBPort_ = (unsigned short)iLBPort;
	}
	
	return 0;
}

// Set up epoll and lisening socket to accept incoming connections or packets
// Run the server in another thread
// Return -1 on Failure
// Return 0 on Success
int SetUpServer(unsigned short usServerPort_)
{
	// Create an Epoll Instance
	int iEpollFD = epoll_create1(0);
	if (-1 == iEpollFD)
	{
		perror("epoll_create1(0)");
		return -1;
	}
	
	// Set up a listening socket to accept incoming connections from clients
	int iListenSockFD = SetUpListenSock(iEpollFD, usServerPort_);
	if (-1 == iListenSockFD)
	{
		printf("SetUpListenSock() failed\n");
		return -1;
	}
	
	pthread_t uiThread;
	g_stArg.iEpollFD = iEpollFD;
	g_stArg.iListenSockFD = iListenSockFD;

	// Create a thread that handles communication with clients
	if (0 != pthread_create(&uiThread, NULL, &ThreadMain, (void*)&g_stArg))
	{
		perror("pthread_create()");
		return -1;
	}
	
	return 0;
}

// Create and set up a TCP listening socket for accepting incoming connection from clients
// Return -1 on Failure
// Return a non-negative integer on Success
int SetUpListenSock(int iEpollFD_, unsigned short usPort_)
{
	int iListenSockFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (-1 == iListenSockFD)
	{
		perror("socket() Server Listen Socket");
		return -1;
	}
	
	if (-1 == SetSocketOptions(iListenSockFD))
		return -1;
	
	struct sockaddr_in stSockAddr;
	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(usPort_);
	stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (-1 ==  bind(iListenSockFD, (struct sockaddr*)&stSockAddr, sizeof(stSockAddr)))
	{
		perror("bind()");
		return -1;
	}
	
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = iListenSockFD;
	
	// Register the socket file descriptor to the epoll file descriptor
	if (-1 == epoll_ctl(iEpollFD_, EPOLL_CTL_ADD, iListenSockFD, &event))
	{
		perror("epoll_ctl() EPOLL_CTL_ADD");
		return -1;
	}
	
	if (-1 == listen(iListenSockFD, SOMAXCONN))
	{
		perror("listen()");
		return -1;
	}
	
	return iListenSockFD;
}

// Set up socket options
// Return -1 on Failure
// Return 0 on Success
int SetSocketOptions(int iSockFD_)
{
	const int enable = 1;
	int iResult = setsockopt(iSockFD_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
	if (-1 == iResult)
	{
		perror("setsockopt() SO_REUSEADDR");
		return -1;	
	}
		
	iResult = setsockopt(iSockFD_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
	if (-1 == iResult)
	{
		perror("setsockopt() SO_REUSEPORT");
		return -1;	
	}
	
	if (-1 == setsockopt(iSockFD_, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)))
	{
		perror("setsockopt() TCP_NODELAY");
		return -1;
	}
	
	return 0;
}

// Communicate with clients in another thread
void* ThreadMain(void* pArg_)
{
	ThreadData* pData = (ThreadData*)pArg_;
	int iEpollFD = pData->iEpollFD;
	int iListenSockFD = pData->iListenSockFD;
	
	// Epoll Events
	struct epoll_event stEPollEvents[MAX_EVENT_COUNTS];
	memset(stEPollEvents, 0, sizeof(stEPollEvents));
	
	do
	{
		// Wait until an event occurs
		int iEventCounts = epoll_wait(iEpollFD, stEPollEvents, MAX_EVENT_COUNTS, -1);
		if (-1 == iEventCounts)
		{
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}
		
		for (int i = 0; i < iEventCounts; ++i)
		{
			if (EpollEventHandler(iEpollFD, iListenSockFD, stEPollEvents[i].data.fd, stEPollEvents[i].events))
			{
				printf("Error in EpollEventHandler()\n");
				exit(EXIT_FAILURE);
			}
		}
	} while (1);
	
	return NULL;
}

// Accept client connectons
// Return -1 on Failure (Terminate)
// Return -2 on Possible Error(Continue to run)
// Return a non-negative integer on Success
int AcceptConnection(int iEpollFD_, int iListenSockFD_)
{
	struct sockaddr_in stSockAddr;
	socklen_t uiAddrLen = sizeof(stSockAddr);
	int iClientFD = accept(iListenSockFD_, (struct sockaddr *)&stSockAddr, &uiAddrLen);
	if (-1 == iClientFD)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return -2;
		
		perror("accept()");
		return -1;
	}
				
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = iClientFD;
	
	// Register the socket file descriptor to the epoll file descriptor
	if (-1 == epoll_ctl(iEpollFD_, EPOLL_CTL_ADD, iClientFD, &event))
	{
		perror("epoll_ctl() EPOLL_CTL_ADD");
		return -1;
	}
				
	++g_iClientCounts;
	
	return iClientFD;
}

// Receive a packet from a client
// Return -1 on Failure
// Return 0 on Success
int RecvClientPacket(int iSockFD_)
{
	int iBuff;
	ssize_t iResult = recv(iSockFD_, &iBuff, sizeof(iBuff), 0);
	if (-1 == iResult)
	{
		perror("recv()");
		return -1;
	}
	
	return 0;
}

// Handle All Epoll events
// Return -1 on Failure (Terminate)
// Return a non negative integer on Success
int EpollEventHandler(int iEpollFD_, int iListenSockFD_, int iEventFD_, uint32_t uiEpollEvent_)
{
	// Error Checking
	if ((EPOLLERR & uiEpollEvent_) ||  (EPOLLHUP & uiEpollEvent_) || (EPOLLRDHUP & uiEpollEvent_))
		return DisconnectHandler(iEpollFD_, iEventFD_);
	else // (EPOLLIN & uiEpollEvent_)
		return EpollInHandler(iEpollFD_, iListenSockFD_, iEventFD_); 
}

// Handle a disconnected client
// Return -1 on Failure
// Return 0 on Success
int DisconnectHandler(int iEpollFD_, int iSockFD_)
{
	// Deregister the Socket from the EPoll descriptor
	struct epoll_event event;
	if (-1 == epoll_ctl(iEpollFD_, EPOLL_CTL_DEL, iSockFD_, &event))
	{
		perror("epoll_ctl EPOLL_CTL_DEL");
		return -1;
	}
		
	close(iSockFD_);
	// The test program could run a large number of clients.
	// If a lot of clients keep running, the machine will become very slow.
	// Thus, the client terminates after making a connection with a server.
	// Servers still consider clients are connected.
	// That means servers do not decrease the value of connected clients
	//--g_iClientCounts;
		
	return 0;
}

// Handle EPOLLIN event
// Return -1 on Failure (Terminate)
// Return -2 on Possible Error( Continue to run)
// Return a non negative integer on Success
int EpollInHandler(int iEpollFD_, int iListenSockFD_, int iEventFD_)
{
	if (iEventFD_ == iListenSockFD_)
	{
		int iCount = 0;
		do
		{
			int iClientSock = AcceptConnection(iEpollFD_, iListenSockFD_);
			if (0 > iClientSock)
				return iClientSock;
			
			++iCount;
		}while (iCount < MAX_CLIENT_ACCEPT_LOOPING_COUNT);
			
		return 0;
	}
	else
		return RecvClientPacket(iEventFD_);
}

// Initiate Connection with the load balancer
// Return -1 on Failure (Terminate)
// Return a non negative integer on Success
int ConnectToLoadBalancer(in_addr_t uiIP_, unsigned short usPort_)
{
	// TCP Socket to communicate with the load balancer
	int iSockFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (-1 == iSockFD)
	{
		perror("socket() Load Balancer TCP Socket");
		return -1;
	}
	
	const int enable = 1;
	if (-1 == setsockopt(iSockFD, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)))
	{
		perror("TCP_NODELAY Failed!");
		return -1;
	}

	struct sockaddr_in stSockAddr;
	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(usPort_);
	stSockAddr.sin_addr.s_addr = uiIP_;

	
	// Connect to the load balancer
	if (-1 == connect(iSockFD, (struct sockaddr*)&stSockAddr, sizeof(stSockAddr)))
	{
		perror("connect() to load balancer");
		return -1;
	}
	
	return iSockFD;
}


// Send the Load Balancer the port number on which the server is listening 
// Return -1 on Failure 
// Return a non negative integer on Success
int SendServerPort(int iLBSockFD_, unsigned short usServerPort_)
{
	const size_t uiSendBuffLength = PACKET_TYPE_LENGTH * SERVER_PORT_NUM_PACKET_DATA_LENGTH;
	unsigned char szSendBuff[uiSendBuffLength];
	unsigned short* pSendPacket = (unsigned short*)szSendBuff;
	
	*pSendPacket = SERVER_PORT_NUM_PACKET_TYPE;
	*(pSendPacket + 1) = usServerPort_;
		
	ssize_t iResult = send(iLBSockFD_, szSendBuff, uiSendBuffLength, 0);
	if (-1 == iResult)
	{
		perror("send() to load balaner");
		return -1;
	}
	
	return iResult;
}

// Send the load balancer the number of clients currently connected to the server
// The number of connected clients repsents how busy the server is 
// Return -1 on Failure (Terminate)
// Return a non negative integer on Success
int SendServerStatus(int iLBSockFD_)
{
	const size_t uiSendBuffLength = PACKET_TYPE_LENGTH + SERVER_STATUS_UPDATE_PACKET_DATA_LENGTH;
	unsigned char szSendBuff[uiSendBuffLength];
	unsigned short* pPacketType = (unsigned short*)szSendBuff;
	
	*pPacketType = SERVER_STATUS_UPDATE_PACKET_TYPE;
	long int* pClientCount = (long int*)(pPacketType + 1);
	*pClientCount = g_iClientCounts;
		
	ssize_t iResult = send(iLBSockFD_, szSendBuff, uiSendBuffLength, 0);
	/*
	// For testing, this error hadnling is commented to get clear logs
	if (-1 == iResult)
	{
		perror("send() to load balaner");
		return -1;
	}
	*/
	return iResult;
}


// Handling SIGPIPE signal (for testing)
void SignalHandler(int iSignal_)
{
	OnExit();
}

// Print out the number of clients currently connected
void OnExit()
{
	printf("%ld Clients are connected\n", g_iClientCounts);
}


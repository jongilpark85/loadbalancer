#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include "Common_Header.h"

// Default IP of the load balancer (For testing)
#define DEFAULT_LB_IP "127.0.0.1"

// Get Command Line Arguments if provided
// load balancer IP, load balancer port 
int ParseArguments(int argc, char* argv[], struct in_addr* pLB_IP_, unsigned short* pLBPort_);

// Create a UDP socket and make it non-blocking
int SetUpUDPsocket();
	
// Initiate Connection to the server
int ConnectToServer(in_addr_t uiIP_, unsigned short usPort_);

// Get the IP and port of a server from the load balancer
int GetServerAddr(int iUDPSockFD_, in_addr_t uiLBIP_, unsigned short usLBPort_, in_addr_t* pServerIP_, unsigned short* pServerPort_);

// Send Server Addr Request to the load balancer
ssize_t SendServerAddrReq(int iLBSockFD_, in_addr_t uiLBIP_, unsigned short usLBPort_);

// Receive Server Addr Response from the load balancer
ssize_t RecvServerAddrResponse(int iLBSockFD_, in_addr_t* pIP_, unsigned short* pPort_);

// Communicate with the server
// Currently, the client simply sends random data in a loop 
int CommunicateWithServer(int iServerSockFD_);

// Handling SIGPIPE signal (For testing)
void SignalHandler(int iSignal_);

// Print out a message (for testing)
void OnExit();

// Main Function
int main(int argc, char *argv[])
{
	// For testing
	//signal(SIGPIPE, SignalHandler);
	//atexit(OnExit);

	unsigned short usLBPort = LB_PORT_FOR_CLIENT; // Load Balancer Port
	struct in_addr stLB_IP; // Load Balancer IP
	
	// Get Command Line Arguments
	if (-1 == ParseArguments(argc, argv, &stLB_IP, &usLBPort))
		exit(EXIT_FAILURE);
	
	// Set up UDP socket to communicate with the load balancer
	int iLBSockFD = SetUpUDPsocket();
	if (-1 == iLBSockFD)
		exit(EXIT_FAILURE);
	
	
	in_addr_t uiServerIP;
	unsigned short usServerPort;
	// Get the IP and Port of a server from the Load balaner
	if (-1 == GetServerAddr(iLBSockFD, stLB_IP.s_addr, usLBPort, &uiServerIP, &usServerPort))
	{
		printf("Error in GetServerAddr()\n");
		exit(EXIT_FAILURE);
	}
	
	// Initiate connection to the server
	int iServerSockFD = ConnectToServer(uiServerIP, usServerPort);
	if (-1 == iServerSockFD)
	{
		printf("Error in ConnectToServer()\n");
		exit(EXIT_FAILURE);
	}

	/*
	// The test program could run a large number of clients.
	// If a lot of clients keep running, the machine will become very slow.
	// Thus, the client terminates after making a connection with a server.
	// Servers still consider clients are connected.
	// That means servers do not decrease the value of connected clients
	// Communicate with the server
	// Currently, the client simply sends random data in a loop 
	if (-1 == CommunicateWithServer(iServerSockFD))
		exit(EXIT_FAILURE);
	
	*/
	return 0;
}


// Get Command Line Arguments if provided
// load balancer IP, load balancer port 
// Return -1 on Failure
// Return 0 on Success
int ParseArguments(int argc, char* argv[], struct in_addr* pLB_IP_, unsigned short* pLBPort_)
{
	if (2 <= argc)
	{
		if (0 == inet_aton(argv[1], pLB_IP_))
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
	
	if (3 <= argc)
	{
		int iLBPort = atoi(argv[2]);
		if (iLBPort < 0 || 65535 < iLBPort)
		{
			printf("Load balancer Invalid Port Number\n");
			return -1;
		}
		
		*pLBPort_ = (unsigned short)iLBPort;
	}
	
	return 0;
}

// Create a UDP socket and make it non-blocking
// Return -1 on Failure
// Return a non-negative integer on Success
int SetUpUDPsocket()
{
	// UDP Socket to communicate with the load balancer
	int iUDPSockFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); 
	if (-1 == iUDPSockFD)
	{
		perror("socket() UDP for load balancer");
		return -1;
	}
	
	int iFlags = fcntl(iUDPSockFD, F_GETFL, 0);
	if (-1 == iFlags)
	{
		perror("fcntl() F_GETFL");
		return -1;
	}
	iFlags |= O_NONBLOCK;
	
	if (-1 == fcntl(iUDPSockFD, F_SETFL, iFlags))
	{
		perror("fcntl() F_SETFL");
		return -1;
	}
	
	return iUDPSockFD;
}

// Get the IP and port of a server from the load balancer
// Return -1 on Failure
// Return 0 on Success
int GetServerAddr(int iLBSockFD_, in_addr_t uiLBIP_, unsigned short usLBPort_, in_addr_t* pServerIP_, unsigned short* pServerPort_)
{
	// UDP does not provide delivery guarantee, so there needs retransmission.
	// This is just a test client example, so the client simply sends, sleeps, and receives in a loop until it gets a response from the load balancer
	while (1)
	{
		// Send Server Addr Request to the load balancer
		ssize_t iResult = SendServerAddrReq(iLBSockFD_, uiLBIP_, usLBPort_);
		if (-1 == iResult)
			return -1;
		else if (0 == iResult)
			continue;
		
		sleep(1);
		
		// Receive Server Addr Response from the load balancer
		iResult = RecvServerAddrResponse(iLBSockFD_, pServerIP_, pServerPort_);
		if (-1 == iResult)
			return -1;
		else if (0 == iResult)
			continue;

		break;
	}

	return 0;
}

// Send Server Addr Request to the load balancer
// Return -1 on Failure
// Return 0 if space is not available
// Return an non-negative integer on Success
ssize_t SendServerAddrReq(int iLBSockFD_, in_addr_t uiLBIP_, unsigned short usLBPort_)
{
	struct sockaddr_in stSockAddr;
	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(usLBPort_);
	stSockAddr.sin_addr.s_addr = uiLBIP_;
	socklen_t uiAddrLen = sizeof(stSockAddr);
	
	unsigned char szSendBuff[REQUEST_FROM_CLIENT_LENGTH];
	*(unsigned short*)szSendBuff = SERVER_ADDR_REQUEST_TYPE;
	ssize_t iResult = sendto(iLBSockFD_, szSendBuff, REQUEST_FROM_CLIENT_LENGTH, 0, (struct sockaddr*)&stSockAddr, uiAddrLen);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("send() to load balaner");
		return -1;
	}
	
	return iResult;
}

// Receive Server Addr Response from the load balancer
// Return -1 on an erorr or unepected result 
// Return 0 if no data in the socket buffer
// Return 1 on success
ssize_t RecvServerAddrResponse(int iLBSockFD_, in_addr_t* pIP_, unsigned short* pPort_)
{
	struct sockaddr_in stSockAddr;
	socklen_t uiAddrLen = sizeof(stSockAddr);
	
	unsigned char* szRecvBuff[RESPONSE_TO_CLIENT_LENGTH];
	ssize_t iResult = recvfrom(iLBSockFD_, szRecvBuff, RESPONSE_TO_CLIENT_LENGTH, 0, (struct sockaddr*)&stSockAddr, &uiAddrLen);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("recv() from loadbalancer");
		return -1;
	}
	else if (iResult < RESPONSE_TO_CLIENT_LENGTH)
	{
		printf("recvfrom() unexpected result\n");
		return -1;
	}

	unsigned short* pPacket = (unsigned short*)szRecvBuff;
	unsigned short usType = *pPacket;
	if (SERVER_ADDR_REQUEST_TYPE != usType)
	{
		printf("Unexpected Response\n");
		return -1;
	}
	
	unsigned short usErrorCode = *(pPacket + 1);
	
	if (SERVER_ADDR_RESPONSE_SUCCESS == usErrorCode)
	{
		*pPort_ = *(pPacket + 2);
		*pIP_ = *((in_addr_t*)(pPacket + 3));
		
		return 1;
	}
	else if (SERVER_ADDR_RESPONSE_NO_SERVER == usErrorCode)
	{
		printf("There is currently no server available\n");
		return -1;
	}
	else if (SERVER_ADDR_RESPONSE_UNKNOWN_TYPE == usErrorCode)
	{
		printf("The packet this client sent to the load balancer is invalid\n");
		return -1;
	}
	else
	{
		printf("Unexpected Response from the load balancer\n");
		return -1;
	}
	
	return 1;
}

// Initiate Connection to the server
// Return -1 on Failure
// Return an non-negative integer on Success
int ConnectToServer(in_addr_t uiIP_, unsigned short usPort_)
{
	// TCP socket to communicate with the server
	int iServerSockFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); 
	if (-1 == iServerSockFD)
	{
		perror("socket() for server");
		return -1;
	}
		
	const int enable = 1;
	if (-1 == setsockopt(iServerSockFD, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)))
	{
		perror("TCP_NODELAY Failed!");
		return -1;
	}
	
	struct sockaddr_in stServerAddr;
	stServerAddr.sin_family = AF_INET;
	stServerAddr.sin_port = htons(usPort_);
	stServerAddr.sin_addr.s_addr = uiIP_;
	
	do
	{
		if (-1 == connect(iServerSockFD, (struct sockaddr*)&stServerAddr, sizeof(stServerAddr)))
		{
			if (ECONNREFUSED == errno)
				continue;
			
			perror("connect() to server");
			return -1;
		}
		
		break;
	} while (1);
	
	return iServerSockFD;
}

// Communicate with the server
// Currently, the client simply sends random data in a loop
// Return -1 on Failure
// Return 0 on Success
int CommunicateWithServer(int iServerSockFD_)
{
	do
	{
		int iTest = rand();
		ssize_t iResult = send(iServerSockFD_, &iTest, sizeof(iTest), 0);
		
		if (-1 == iResult)
		{
			perror("send()");
			return -1;
		}
		
		
		sleep(rand() % 10 + 1);
	} while (1);
	
	return 0;
}

// Handling SIGPIPE signal (for testing)
void SignalHandler(int iSignal_)
{
	OnExit();
}

// Print out a message (for testing)
void OnExit()
{
	printf("Client has been terminated\n");
}
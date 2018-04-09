#include "CLoadBalancer.h"

// Since each thread manages different servers and clients, 
// the update operation of the server state can be done lockfree.
// In addition, when the loadbalancer chooses the best server for a client, 
// it acceses the server state information only with read operations.
// Therefore, load balancing is also done lockfree.

// The number of servers connected to each thread of this Loadbalancer.
long g_uiServerCounts[MAX_THREAD_COUNTS] = { 0 };

// Information used for load balancing
// The number of clients currently connected to each server

Simple_List<long int*>* g_pClientCountsList[MAX_THREAD_COUNTS] = { 0 };

// Other information can be used for load balancing as well.
// For example, the number of requests that each server has received from clients for a certain time period.
//Simple_List<long int*>* g_pRequestCountsList[MAX_THREAD_COUNTS] = { 0 };

// Server information including each server's IP, port, and socket descriptor.
Simple_List<Server_Address_Info*>* g_pServerInfoList[MAX_THREAD_COUNTS] = {0};


// Constructor
// Set up Port Numbers servers and clients connect to
CLoadBalancer::CLoadBalancer(__uint16_t uiPort1_, __uint16_t uiPort2_, int iThreadIndex_)
{
	m_usPortForClients = uiPort1_;
	m_uiPortForServers = uiPort2_;
	
	m_iThreadIndex = iThreadIndex_;
	m_iEPollFD = -1;
	
	m_uiPacketDataLength[SPT_PORT] = SERVER_PORT_NUM_PACKET_DATA_LENGTH;
	m_uiPacketDataLength[SPT_STATUS] = SERVER_STATUS_UPDATE_PACKET_DATA_LENGTH;
	
	AllocateMemoryForNewServers();
}

// Destructor
CLoadBalancer::~CLoadBalancer()
{
	// The life span of a CLoadBalancer instance equals to that of the load balancer.
	// When the load balancer is terminated due to an error,  the linux kernel will clean up memory and socket file descriptors.
	// Thus, there needs no explicit resource release here.
	// When a client or server is disconnected or when a packet in the queue is completely processed, those resources are freed properly to prevent memory leak or waste.
}

// Set up the Load ballancer
// Set up sockets to accept incoming connections and packets
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SetUp()
{
	m_iEPollFD = epoll_create1(0);
	if (-1 == m_iEPollFD)
	{
		perror("epoll_create1(0)");
		return -1;
	}
	
	//  TCP listening socket for clients
	m_iListenSockForClients = SetUpTCPListenSocket(m_usPortForClients);
	if (-1 == m_iListenSockForClients)
	{
		DisplayErrorMessage("SetUpTCPListenSocket() for Clients Failed");
		return -1;
	}
	
	//  UDP socket for clients
	m_iUDPSockForClients = SetUpUDPSocket(m_usPortForClients);
	if (-1 == m_iUDPSockForClients)
	{
		DisplayErrorMessage("SetUpUDPSocket() for Clients Failed");
		return -1;
	}
		
	//  TCP listening socket for servers
	m_iListenSockForServers = SetUpTCPListenSocket(m_uiPortForServers);
	if (-1 == m_iListenSockForServers)
	{
		DisplayErrorMessage("SetUpTCPListenSocket() for Servers Failed");
		return -1;
	}
		
	return 0;
}

// Display an error message
void CLoadBalancer::DisplayErrorMessage(const char* szErrorMessage_)
{
	printf("THREAD %d, Error : %s\n", m_iThreadIndex, szErrorMessage_);
}


// Create a TCP listening socket and set it up to accept incomming connections.
// Return -1 on Failure
// Return a non-negative integer on Success
int CLoadBalancer::SetUpTCPListenSocket(unsigned short usPort_)
{
	int iSockFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (-1 == iSockFD)
	{
		perror("socket()");
		return -1;
	}
	
	if (-1 == SetUpSocket(iSockFD, usPort_, EPOLLIN | EPOLLRDHUP))
		return -1;
	
	
	if (-1 == listen(iSockFD, SOMAXCONN))
	{
		perror("listen()");
		return -1;
	}
	
	return iSockFD;
}

// Create a UDP socket and set it up to communicate with clients
// Return -1 on Failure
// Return a non-negative integer on Success
int CLoadBalancer::SetUpUDPSocket(unsigned short usPort_)
{
	// Create a UDP socket to communicate with clients
	int iSockFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (-1 == iSockFD)
	{
		perror("socket()");
		return -1;
	}
	
	if (-1 == SetUpSocket(iSockFD, m_usPortForClients, EPOLLIN))
		return -1;
	
	return iSockFD;
}

// Set up socket to be ready for communication with clients and servers
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SetUpSocket(int iSockFD_, unsigned short uiPort_, uint32_t uiEpollEvents_)
{
	// Set up socket options
	if (-1 == SetSocketOptions(iSockFD_))
		return -1;
	
	struct sockaddr_in stSockAddr;
	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons(uiPort_);
	stSockAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	if (-1 == bind(iSockFD_, (struct sockaddr*)&stSockAddr, sizeof(stSockAddr)))
	{
		perror("bind()");
		return -1;
	}
	
	// Make it non-blocking
	if (-1 == SetNonBlocking(iSockFD_))
		return -1;
	
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = uiEpollEvents_;
	event.data.fd = iSockFD_;
	
	// Register the socket file descriptor to the epoll file descriptor
	if (-1 == epoll_ctl(m_iEPollFD, EPOLL_CTL_ADD, iSockFD_, &event))
	{
		perror("epoll_ctl() EPOLL_CTL_ADD");
		return -1;
	}
	
	return 0;
}

// Enable Socket Options
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SetSocketOptions(int iSockFD_)
{
	const int enable = 1;
	if (-1 == setsockopt(iSockFD_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)))
	{
		perror("setsockopt() SO_REUSEADDR");
		return -1;
	}

	// SO_REUSEPORT option is enabled to make every thread equal in functionality.
	// In a traditional way, there are a listening thread and several processing threads.
	// In that case, the listening thread can be a bottleneck.
	// With SO_REUSEPORT, every thread binds its listening socket to the same port.
	// Incomming connections from servers and clients are distributed evenly across all of the threads. 
	if (-1 == setsockopt(iSockFD_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)))
	{
		perror("setsockopt() SO_REUSEPORT");
		return -1;
	}

	return 0;
}

// Make the sock use Non-blocking mode
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SetNonBlocking(int iSockFD_)
{
	int iFlags = fcntl(iSockFD_, F_GETFL, 0);
	if (-1 == iFlags)
	{
		perror("fcntl() F_GETFL");
		return -1;
	}
	iFlags |= O_NONBLOCK;
	
	if (-1 == fcntl(iSockFD_, F_SETFL, iFlags))
	{
		perror("fcntl() F_SETFL");
		return -1;
	}
	
	return 0;
}

// Wrapper for epoll_ctl() 
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::Epoll_CTL_Wrapper(int iOption_, int iSockFD_, unsigned int uiEvent_)
{
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = uiEvent_;
	event.data.fd = iSockFD_;
	
	if (-1 == epoll_ctl(m_iEPollFD, iOption_, iSockFD_, &event))
	{
		perror("epoll_ctl() Failed!");
		return -1;
	}
	
	return 0;
}

// Remove a server from the list when the server gets disconnected
void CLoadBalancer::RemoveServer(int iSockFD_)
{
	std::unordered_map<int, Server_Data_Access_Info*>::iterator mitor = m_mapServerList.find(iSockFD_);
			
	// This is not a server socket
	if (m_mapServerList.end() == mitor)
		return;
	
	Server_Data_Access_Info* pServer = mitor->second;
	int iListIndex = pServer->iListIndex;
	int iArrIndex = pServer->iArrayIndex;
					
	Simple_List<long int*>* pClientCountsList = g_pClientCountsList[m_iThreadIndex];

	int i = 0;
	while (i < iListIndex)
	{
		pClientCountsList = pClientCountsList->pNext;
		++i;
	}
							
	if (-1 != iArrIndex)
		pClientCountsList->Data[iArrIndex] = SERVER_DISCONNECTED;

	
	m_mapServerList.erase(mitor);
				
	return;
}

// Send all of th UDP packets in the queue until space is not available or queue is empty
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SendUDPQueuePacket(int iSockFD_)
{
	if (iSockFD_ != m_iUDPSockForClients)
		return 0;
	
	std::list < Queued_UDP_Packet*>::iterator litor = m_listUDPPacketQueue.begin();
	while (litor != m_listUDPPacketQueue.end())
	{
		Queued_UDP_Packet* pPacket = (*litor);
		ssize_t iResult = sendto(iSockFD_, pPacket->pBuffer, RESPONSE_TO_CLIENT_LENGTH, 0, (struct sockaddr*)&(pPacket->stSockAddr), pPacket->uiAddrLen);
		if (-1 == iResult)
		{
			if (EAGAIN == errno || EWOULDBLOCK == errno)
				return 0;

			perror("send()");
			return -1;
		}
		else if (0 == iResult)
			return 0;
		else if (RESPONSE_TO_CLIENT_LENGTH == iResult)
		{
			delete pPacket->pBuffer;
			delete pPacket;
			litor = m_listUDPPacketQueue.erase(litor);
		}
		else
		{
			// Should not happen when using UDP
			DisplayErrorMessage("sendto() Unepected Result");
			return -1;
		}
	}
	
	return Epoll_CTL_Wrapper(EPOLL_CTL_MOD, iSockFD_, EPOLLIN);
}

// Handle an EPOLLIN event
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::EpollOutEventHanlder(int iSockFD_)
{
	// If there is a pending packet in the send queue, send it here.
	if (-1 == SendTCPQueuePacket(iSockFD_))
	{
		DisplayErrorMessage("SendTCPQueuePacket() Failed");
		return -1;
	}
	
	if( SendUDPQueuePacket(iSockFD_))
	{
		DisplayErrorMessage("SendUDPQueuePacket() Failed");
		return -1;
	}
	
	return 0;
}


// Handle EPOLLIN event
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::EpollInEventHandler(int iSockFD_)
{
	// UDP
	if (iSockFD_ == m_iUDPSockForClients) // UDP 
	{
		return ClientUDPPacketHandler(m_iUDPSockForClients);
	}
	else if (iSockFD_ == m_iListenSockForClients) // Accept an incoming TCP connection from a client
	{
		int iCount = 0;
		do
		{
			struct sockaddr_in stSockAddr;
			socklen_t uiAddrLen = sizeof(stSockAddr);
				
			// Accepting an incoming connection may fail. ( ex) reached the system limit on the total number of open sockets.
			// The load balancer should keep running
			int iClientSock = AcceptConnection(m_iListenSockForClients, &stSockAddr, &uiAddrLen);
			if (0 > iClientSock )
				return iClientSock;

			++iCount;
		} while (iCount < MAX_CLIENT_ACCEPT_LOOPING_COUNT);
		
		return 0;
	}
	else if (iSockFD_ == m_iListenSockForServers) // Accept an incoming TCP connection from a server
	{
		int iCount = 0;
		do
		{
			struct sockaddr_in stSockAddr;
			socklen_t uiAddrLen = sizeof(stSockAddr);
			int iServerSock = AcceptConnection(m_iListenSockForServers, &stSockAddr, &uiAddrLen);
			if (0 > iServerSock)
				return iServerSock;
			
			struct Server_Data_Access_Info* pServerSocketInfo = new Server_Data_Access_Info;
			pServerSocketInfo->iSocketFD = iServerSock;
			pServerSocketInfo->iArrayIndex = -1;
			pServerSocketInfo->iListIndex  = -1;
			pServerSocketInfo->uiIP = stSockAddr.sin_addr.s_addr;
			m_mapServerList.insert(std::make_pair(iServerSock, pServerSocketInfo));
			++iCount;
			
		} while (iCount < MAX_SERVER_ACCEPT_LOOPING_COUNT);
		
		return 0;
	}
	else
	{
		std::unordered_map<int, Server_Data_Access_Info*>::iterator mitor = m_mapServerList.find(iSockFD_);

		// This is a client socket
		if (m_mapServerList.end() == mitor)
			return ClientTCPPacketHandler(iSockFD_);
		else // This is a server socket
			return ServerPacketHandler(mitor->second);
					
	}
	
	return 0;
}

// Main Loop
// Handle all the communications with servers and clients.
// Never return unless an error occurs.
// If an error occurs in Run(), the load balancer will terminate by calling exit(EXIT_FAILURE).
void CLoadBalancer::Run()
{
	// Epoll Events
	struct epoll_event stEPollEvents[MAX_EVENT_COUNTS];
	memset(stEPollEvents, 0, sizeof(stEPollEvents));
	
	do
	{
		// Wait until an event occurs
		int iEventCounts = epoll_wait(m_iEPollFD, stEPollEvents, MAX_EVENT_COUNTS, -1);
		if (-1 == iEventCounts)
		{
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}
		
		for (int i = 0; i < iEventCounts; ++i)
		{
			// Error Checking
			if ((EPOLLERR & stEPollEvents[i].events) || 
				(EPOLLHUP & stEPollEvents[i].events) || 
				(EPOLLRDHUP & stEPollEvents[i].events))
			{
				// Either a client or server got disconnected
				if (-1 == DisconnectHandler(stEPollEvents[i].data.fd))
				{
					DisplayErrorMessage("DisconnectHandler() Failed");
					exit(EXIT_FAILURE);
				}

			}
			else if (EPOLLOUT & stEPollEvents[i].events)
			{
				if (-1 == EpollOutEventHanlder(stEPollEvents[i].data.fd))
				{
					DisplayErrorMessage("EpollOutEventHanlder() Failed");
					exit(EXIT_FAILURE);
				}
			}
			else // EPOLLIN & stEPollEvents[i].events
			{
				if (-1 == EpollInEventHandler(stEPollEvents[i].data.fd))
				{
					DisplayErrorMessage("EpollOutEventHanlder() Failed");
					exit(EXIT_FAILURE);
				}
			}
		}
	} while (1);
	
	return;
}

// Handle a disconnected client or server
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::DisconnectHandler(int iSockFD_)
{
	RemoveServer(iSockFD_);
	
	// Deregister the Socket from the EPoll descriptor
	struct epoll_event event;
	if (-1 == epoll_ctl(m_iEPollFD, EPOLL_CTL_DEL, iSockFD_, &event))
	{
		perror("epoll_ctl EPOLL_CTL_DEL");
		return -1;
	}
	
	close(iSockFD_);
	
	return 0;
}

// Accept an incoming connection and register the socket to the epoll descriptor
// Return -1 on failure (-1 causes the load balancer to terminate)
// Return a non-negative integer on Success
// Return -2 on Possible Failure ( the load balancer does not terminate)
int CLoadBalancer::AcceptConnection(int iListenSockFD_, sockaddr_in* pSockAddr_, socklen_t* pAddrLen_)
{
	int iSockFD = accept(iListenSockFD_, (struct sockaddr *)pSockAddr_, pAddrLen_);
	if (-1 == iSockFD)
	{
		// Actually, there are more cases where the load balancer should keep running even on accept() failture besides EGAIN or EWOULDBLOCK
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return -2;
			
		perror("accept()");
		return -1;
	}
				
	if (-1 == SetNonBlocking(iSockFD))
		return -1;
	
	
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLRDHUP;
	event.data.fd = iSockFD;
	
	if (-1 == epoll_ctl(m_iEPollFD, EPOLL_CTL_ADD, iSockFD, &event))
	{
		perror("epoll_ctl() EPOLL_CTL_ADD");
		return -1;
	}
	
	return iSockFD;
}

// Send a TCP packet in the queue,  which contains TCP packets that were sent out partially
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SendTCPQueuePacket(int iSockFD_)
{
	std::unordered_map<int, InComplete_Packet*>::iterator mitor = m_mapTCPPacketSendQueue.find(iSockFD_);
	if (m_mapTCPPacketSendQueue.end() == mitor)
		return Epoll_CTL_Wrapper(EPOLL_CTL_MOD, iSockFD_, EPOLLIN | EPOLLRDHUP);

	InComplete_Packet* pPacket = mitor->second;
	size_t uiRemainBytes = pPacket->uiBufferLen - pPacket->uiOffset;
	ssize_t iResult = send(iSockFD_, pPacket->pBuffer + pPacket->uiOffset, uiRemainBytes, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
	
		perror("send()");
		return -1;
	}
	else if ((size_t)iResult < uiRemainBytes)
	{
		pPacket->uiOffset += iResult;
		return 0;
	}
	else
	{
		RemoveTCPSendQueuePacket(iSockFD_, pPacket);
		return Epoll_CTL_Wrapper(EPOLL_CTL_MOD, iSockFD_, EPOLLIN | EPOLLRDHUP);
	}

	return 0;
}

// Send the IP and Port of the least busy server to the client
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::SendResponseToClient(int iSockFD_, unsigned char* szRecvBuff_)
{
	unsigned char szSendBuff[RESPONSE_TO_CLIENT_LENGTH];
	BuildResponse(szRecvBuff_, szSendBuff);

	ssize_t iResult = send(iSockFD_, szSendBuff, RESPONSE_TO_CLIENT_LENGTH, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			iResult = 0;
		else
		{
			perror("send()");
			return -1;
		}
			
	}
	
	if (iResult < RESPONSE_TO_CLIENT_LENGTH)
	{
		AddTCPPacketToSendQueue(iSockFD_, szSendBuff + iResult, RESPONSE_TO_CLIENT_LENGTH - iResult);
		return Epoll_CTL_Wrapper(EPOLL_CTL_MOD, iSockFD_, EPOLLIN | EPOLLRDHUP | EPOLLOUT);		
	}
	
	return 0;
}

// Add a partial TCP packet to the send queue in order to send the rest of the packet when space is availabe
void CLoadBalancer::AddTCPPacketToSendQueue(int iSocket_, unsigned char* pSendBuff_, size_t uiBuffLength_)
{
	InComplete_Packet* pPacket = new InComplete_Packet;
	pPacket->uiOffset = 0;
	pPacket->uiBufferLen = uiBuffLength_;
	pPacket->pBuffer = new unsigned char[uiBuffLength_];
	memcpy((void*)pPacket->pBuffer, (void*)pSendBuff_, uiBuffLength_);
	m_mapTCPPacketSendQueue.insert(std::make_pair(iSocket_, pPacket));
}

// Erase a TCP packet from the send queue and release memory
void CLoadBalancer::RemoveTCPSendQueuePacket(int iSockFD_, InComplete_Packet* pPacket_)
{
	delete pPacket_->pBuffer;
	delete pPacket_;
	m_mapTCPPacketSendQueue.erase(iSockFD_);
}

// Build a response that will be sent to the Client
void CLoadBalancer::BuildResponse(unsigned char* szRecvBuff_, unsigned char* szSendBuff__)
{			
	unsigned short usPacketType = *((unsigned short*)szRecvBuff_);
	unsigned short* pSendPacket = (unsigned short*)szSendBuff__;
	*pSendPacket = usPacketType;
	
	//Checking Received Data from a client
	if (SERVER_ADDR_REQUEST_TYPE != usPacketType)
	{
		// Received a worng format of packet from a client.
		*(pSendPacket + 1) = SERVER_ADDR_RESPONSE_UNKNOWN_TYPE;
		return;
	}
	
	// Choose the least busy server
	int iThreadIndex = -1;
	int iListIndex = -1;
	int iArrIndex = -1;
	GetBestServer(&iThreadIndex, &iListIndex, &iArrIndex);
	
	if (-1 == iThreadIndex)
	{
		//There is no running server
		*(pSendPacket + 1) = SERVER_ADDR_RESPONSE_NO_SERVER;
		return;
	}
		
	
	*(pSendPacket + 1) = SERVER_ADDR_RESPONSE_SUCCESS;
	// Filling the send buffer with the IP and Port of the least busy server
	GetServerAddr((unsigned char*)(pSendPacket + 2), iThreadIndex, iListIndex, iArrIndex);
	
	return;
}

// Get IP and Port of the Server corresponding to the indices
void CLoadBalancer::GetServerAddr(unsigned char* pBuff_, int iThreadIndex_, int iListIndex_, int iArrIndex_)
{
	Simple_List<Server_Address_Info*>* pServerInfoList = g_pServerInfoList[iThreadIndex_];
	int i = 0;
	while (i < iListIndex_)
	{
		if (NULL == pServerInfoList)
			return;
		
		pServerInfoList = pServerInfoList->pNext;
		++i;
	}
		
	unsigned short* pPort = (unsigned short*)pBuff_;
	*pPort = pServerInfoList->Data[iArrIndex_].usPort;
	
	in_addr_t* pIP = (in_addr_t*)(pPort + 1);
	*pIP = pServerInfoList->Data[iArrIndex_].uiIP;

	return;
}

// Choose the server with the fewest clients among all the servers
void CLoadBalancer::GetBestServer(int* pThreadIndex_, int* pListIndex_, int* pArrIndex_)
{
	int iBestThreadIndex = -1;
	int iBestListIndex = -1;
	int iBestArrayIndex = -1;
	
	long int uiMinClinetCounts = LONG_MAX;
	
	for (int i = 0; i < MAX_THREAD_COUNTS; ++i)
	{
		unsigned long uiServerCounts = g_uiServerCounts[i];
		
		Simple_List<long int*>* pClientCountsList = g_pClientCountsList[i];
	
		unsigned long int j = 0;
		int iArrayIndex = 0;
		int iListIndex = 0;
		
		while (j < uiServerCounts)
		{
			long int iClientCounts = pClientCountsList->Data[iArrayIndex];
			if (0 <= iClientCounts && iClientCounts < uiMinClinetCounts)
			{
				uiMinClinetCounts = iClientCounts;
				iBestListIndex = iListIndex;
				iBestArrayIndex = iArrayIndex;
				iBestThreadIndex = i;
			}
			
			if (++iArrayIndex >= MAX_SERVER_NUMS_PER_ARRAY)
			{
				iArrayIndex = 0;
				++iListIndex;
				pClientCountsList = pClientCountsList->pNext;
				
				if (NULL == pClientCountsList)
					break;
			}
			
			++j;
		}
	}
	
	*pArrIndex_ = iBestArrayIndex;
	*pListIndex_ = iBestListIndex;
	*pThreadIndex_ = iBestThreadIndex;
}

// Receive data from a server (TCP)
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::ServerPacketHandler(Server_Data_Access_Info* pServerInfo_)
{
	// It is possible to receive fewer bytes,
	// Check whether there is any data previously received.
	// If so, combine with newly received data with that previous one.
	int iSockFD = pServerInfo_->iSocketFD;
	std::unordered_map<int, InComplete_Packet*>::iterator mitor = m_mapTCPPacketRecvQueue.find(iSockFD);

	// There are some data previously received
	if (m_mapTCPPacketRecvQueue.end() != mitor)
	{
		InComplete_Packet* pInCompletePacket = mitor->second;
		return RecvServerPacketWithPreData(pServerInfo_, pInCompletePacket);			
	}
	// No previous data
	else
	{
		return RecvServerPacket(pServerInfo_);
	}
}

// Receive data from a server and add it to the previous data partially received (TCP)
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::RecvServerPacketWithPreData(Server_Data_Access_Info* pServerInfo_, InComplete_Packet* pInCompletePacket_)
{
	// Receive data from where it left off
	int iSockFD = pServerInfo_->iSocketFD;
	size_t uiRestBytes = pInCompletePacket_->uiBufferLen - pInCompletePacket_->uiOffset;
	ssize_t iResult = recv(iSockFD, pInCompletePacket_->pBuffer + pInCompletePacket_->uiOffset, uiRestBytes, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("recv");
		return -1;
	}
	else if ((size_t)iResult < uiRestBytes)
	{
		// There are more data to receive later
		// For now, store the data that has been received so far 
		pInCompletePacket_->uiOffset += iResult; 
		return 0;
	}
	
	// Add a new Server
	if (SPT_PORT == pInCompletePacket_->iPacketType)
		AddNewServer(pServerInfo_, pInCompletePacket_->pBuffer);
	//Update Server Status
	else if(SPT_STATUS == pInCompletePacket_->iPacketType)
		UpdateServerStatus(pServerInfo_, pInCompletePacket_->pBuffer);
	// Data received above is the header section of a packet
	else
	{
		// A server packet consists of a header section and variable sized data section
		// Get the size of the data section of the packet
		int iPacketType = GetPacketType(pInCompletePacket_->pBuffer);
		if (SPT_MAX == iPacketType)
		{
			RemoveTCPRecvQueuePacket(iSockFD, pInCompletePacket_);
			return DisconnectHandler(iSockFD);
		}
		
		const size_t uiDataLength = GetPacketDataLength(iPacketType);
		unsigned char szRecvBuff[uiDataLength];
		
		// Receive the data section
		iResult = recv(iSockFD, szRecvBuff, uiDataLength, 0);
		if (-1 == iResult)
		{
			if (EAGAIN == errno || EWOULDBLOCK == errno)
				iResult = 0;
			else
			{
				perror("recv");
				return -1;
			}
		}
		
		if ((size_t)iResult < uiDataLength)
		{
			// There are more data to receive later
			// For now, store the data that has been received so far 
			if (uiDataLength > pInCompletePacket_->uiBufferLen)
			{
				delete pInCompletePacket_->pBuffer;
				pInCompletePacket_->pBuffer = new unsigned char[uiDataLength];
			}

			pInCompletePacket_->iPacketType = iPacketType;
			pInCompletePacket_->uiBufferLen = uiDataLength;
			pInCompletePacket_->uiOffset = iResult;

			memcpy((void*)pInCompletePacket_->pBuffer, (void *)szRecvBuff, iResult);
			
			return 0;
		}
		// Data senction has completely been recevied
		else
		{
			// Add a new Server
			if (SPT_PORT == iPacketType)
				AddNewServer(pServerInfo_, szRecvBuff);
			//Update Server Status
			else if (SPT_STATUS == iPacketType)
				UpdateServerStatus(pServerInfo_, szRecvBuff);
			
			RemoveTCPRecvQueuePacket(iSockFD, pInCompletePacket_);
			return 0;
		}
	}
	
	
	RemoveTCPRecvQueuePacket(iSockFD, pInCompletePacket_);
	
	return 0;
}

// Receive data from a server when there is no previous data partially received (TCP)
// Return -1 on Failure
// Return 0 on Success
int CLoadBalancer::RecvServerPacket(Server_Data_Access_Info* pServerInfo_)
{
	int iSockFD = pServerInfo_->iSocketFD;
	unsigned char szHeader[PACKET_TYPE_LENGTH] = { 0, };
	// Receive Packeet Header first
				
	ssize_t iResult = recv(iSockFD, szHeader, PACKET_TYPE_LENGTH, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("recv() header");
		return -1;
	}
	
	// There are more data to receive later
	// For now, store the data that has been received so far 
	if (iResult < PACKET_TYPE_LENGTH)
	{
		AddTCPPacketToRecvQueue(iSockFD, SPT_MAX, PACKET_TYPE_LENGTH, iResult, szHeader);
		return 0;
	}
		
	// Receive packet data section
	int iPacketType = GetPacketType(szHeader);
	if (SPT_MAX == iPacketType)
		return DisconnectHandler(iSockFD);
	
	const size_t uiDataLength = GetPacketDataLength(iPacketType);
	unsigned char szRecvBuff[uiDataLength];
	
	iResult = recv(iSockFD, szRecvBuff, uiDataLength, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			iResult = 0;
		else
		{
			perror("recv() data");
			return -1;	
		}
		
	}
								
	// There are more data to receive later
	// For now, store the data that has been received so far 
	if ((size_t)iResult < uiDataLength)
	{
		AddTCPPacketToRecvQueue(iSockFD, iPacketType, uiDataLength, iResult, szRecvBuff);
		return 0;
	}
	
	
	// A whole packet has completely been received
	// Add a new Server
	if (SPT_PORT == iPacketType)
		AddNewServer(pServerInfo_, szRecvBuff);
	//Update Server Status
	else if (SPT_STATUS == iPacketType)
		UpdateServerStatus(pServerInfo_, szRecvBuff);
			
	return 0;
}

// Receive data from a client (TCP)
// Return 0 on Failure
// Return 1 on Success
int CLoadBalancer::ClientTCPPacketHandler(int iSockFD_)
{
	// It is possible to receive fewer bytes.
	// Check whether there is any data previously received.
	// If so, combine with newly received data with that previous one.
	std::unordered_map<int, InComplete_Packet*>::iterator mitor = m_mapTCPPacketRecvQueue.find(iSockFD_);

	// There are some data previously received
	if (m_mapTCPPacketRecvQueue.end() != mitor)
	{
		InComplete_Packet* pInCompletePacket = mitor->second;
		return RecvClientPacketWithPreData(iSockFD_, pInCompletePacket);			
	}
	// No previous data
	else
	{
		return RecvClientPacket(iSockFD_);
	}
		
	return 0;
}

// Receive data from a client and add it to the previous data partially received (TCP)
// Return 0 on Failure
// Return 1 on Success
int CLoadBalancer::RecvClientPacketWithPreData(int iSockFD_, InComplete_Packet* pInCompletePacket_)
{
	size_t uiRestBytes = pInCompletePacket_->uiBufferLen - pInCompletePacket_->uiOffset;
	ssize_t iResult = recv(iSockFD_, pInCompletePacket_->pBuffer + pInCompletePacket_->uiOffset, uiRestBytes, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("recv");
		return -1;
	}
	else if ((size_t)iResult < uiRestBytes)
	{
		// There are more data to receive later
		// For now, store the data that has been received so far 
		pInCompletePacket_->uiOffset += iResult; 
		return 0;
	}
	
	// Send a response with the best available server's IP and Port back to the client.
	if (-1 == SendResponseToClient(iSockFD_, pInCompletePacket_->pBuffer))
		return -1;
	
	RemoveTCPRecvQueuePacket(iSockFD_, pInCompletePacket_);
	
	return 0;
}

// Receive data from a client when there is no previous data partially received (TCP)
// Return 0 on Failure
// Return 1 on Success
int CLoadBalancer::RecvClientPacket(int iSockFD_)
{
	unsigned char szRecvBuff[REQUEST_FROM_CLIENT_LENGTH] = { 0, };
	// Receive Packeet Header first
	ssize_t iResult = recv(iSockFD_, szRecvBuff, REQUEST_FROM_CLIENT_LENGTH, 0);
	if (-1 == iResult)
	{
		if (EAGAIN == errno || EWOULDBLOCK == errno)
			return 0;
		
		perror("recv");
		return -1;
	}
	else if (iResult < REQUEST_FROM_CLIENT_LENGTH)
	{
		// There are more data to receive later
		// For now, store the data that has been received so far 
		AddTCPPacketToRecvQueue(iSockFD_, -1, REQUEST_FROM_CLIENT_LENGTH, iResult, szRecvBuff);
		return 0;
	}
														
	
	// Send a response with the best available server's IP and Port back to the client.
	if (-1 == SendResponseToClient(iSockFD_, szRecvBuff))
		return -1;
	
	return 0;
}


// Receive a UDP packet from a client 
// Return 0 on Failure
// Return 1 on Success
int CLoadBalancer::ClientUDPPacketHandler(int iSockFD_)
{
	// Receive a UDP Request for a Client
	// Mulitple clients send a request to this UDP socket, so there could be multiple packets
	// Read All of them and send a response to each client
	unsigned char szRecvBuff[REQUEST_FROM_CLIENT_LENGTH] = { 0, };
	int iCount = 0;
	do
	{
		struct sockaddr_in stSockAddr;
		memset(&stSockAddr, 0, sizeof(stSockAddr));
		socklen_t uiAddrLen = sizeof(stSockAddr);
						
		ssize_t iReadBytes = recvfrom(m_iUDPSockForClients, szRecvBuff, REQUEST_FROM_CLIENT_LENGTH, 0, (struct sockaddr *)&stSockAddr, &uiAddrLen);
		if (-1 == iReadBytes)
		{
			if (EAGAIN == errno || EWOULDBLOCK == errno)
				break;
			
			perror("recvfrom");
			return -1;
		}
		else if (0 == iReadBytes)
			continue;
		
		// Build a Response and Send it back to the Client
		unsigned char szSendBuff[RESPONSE_TO_CLIENT_LENGTH] = { 0, };
		BuildResponse(szRecvBuff, szSendBuff);
		ssize_t iSendBytes = sendto(m_iUDPSockForClients, szSendBuff, RESPONSE_TO_CLIENT_LENGTH, 0, (struct sockaddr *)&stSockAddr, uiAddrLen);
		if (-1 == iSendBytes)
		{
			if (EAGAIN == errno || EWOULDBLOCK == errno)
				iSendBytes = 0;
			else
			{
				perror("sendto()");
				return -1;
			}
		}
		
		if (0 == iSendBytes)
		{
			Queued_UDP_Packet* pUDPPacket = new Queued_UDP_Packet;
			pUDPPacket->pBuffer = new unsigned char[RESPONSE_TO_CLIENT_LENGTH];
			memcpy(pUDPPacket->pBuffer, szSendBuff, RESPONSE_TO_CLIENT_LENGTH);
			memcpy(&(pUDPPacket->stSockAddr), &stSockAddr, sizeof(pUDPPacket->stSockAddr));
			pUDPPacket->uiAddrLen = uiAddrLen;
			pUDPPacket->uiBufferLen = RESPONSE_TO_CLIENT_LENGTH;
							
			m_listUDPPacketQueue.push_back(pUDPPacket);
			if (-1 == Epoll_CTL_Wrapper(EPOLL_CTL_MOD, iSockFD_, EPOLLIN | EPOLLOUT))
				return -1;
		}
					
	} while (++iCount < MAX_UDP_PACKET_LOOPING_COUNT);
	
	return 0;
}

// Update the status of a server with the new value transferred from that server
void CLoadBalancer::UpdateServerStatus(Server_Data_Access_Info* pServerInfo_, unsigned char* pRecvBuff_)
{
	int iListIndex = pServerInfo_->iListIndex;
	int iArrIndex = pServerInfo_->iArrayIndex;
	
	if (-1 == iListIndex || -1 == iArrIndex)
	{
		DisplayErrorMessage("Unexpected Server Indices");
		return;
	}
							

	int i = 0;
	Simple_List<long int*>* pClientCountsList = g_pClientCountsList[m_iThreadIndex];
	
	while (i < iListIndex)
	{
		pClientCountsList = pClientCountsList->pNext;
		++i;
	}
	
	long int* pData = (long int*)(pRecvBuff_);
	long int iNewClinetCounts = *(pData);
	
	pClientCountsList->Data[iArrIndex] = iNewClinetCounts;
}

// Get the type of a packet
int CLoadBalancer::GetPacketType(unsigned char* pRecvBuff_)
{
	unsigned short usType = *((unsigned short*)pRecvBuff_);
	if (SERVER_STATUS_UPDATE_PACKET_TYPE == usType)
		return SPT_STATUS;
	else if (SERVER_PORT_NUM_PACKET_TYPE == usType)
		return SPT_PORT;
	else
		return SPT_MAX;
}

// Get the length of the data section of a packet
size_t CLoadBalancer::GetPacketDataLength(int iPacketType_)
{
	return m_uiPacketDataLength[iPacketType_];
}
	
// Allocate memory to store information about the new servers
// Frequent memory allocation could increase overhead, so memory for MAX_SERVER_NUMS_PER_ARRAY (20) servers are allocated at once.
// No memory allocation is needed until the number of servers exceeds MAX_SERVER_NUMS_PER_ARRAY value
void CLoadBalancer::AllocateMemoryForNewServers()
{
	unsigned long uiServerCounts = g_uiServerCounts[m_iThreadIndex];
	int iArrIndex = uiServerCounts % MAX_SERVER_NUMS_PER_ARRAY;
	int iListIndex = uiServerCounts / MAX_SERVER_NUMS_PER_ARRAY;
	
	// Allocate memory only when the array is out of space
	if (0 != iArrIndex)
		return;
	
	// Allocate Memory
	Simple_List<long int*>* pNewClientCountsList = new Simple_List<long int*>;
	pNewClientCountsList->Data = new long int[MAX_SERVER_NUMS_PER_ARRAY];
	memset(pNewClientCountsList->Data, SERVER_NEVER_CONNECTED, MAX_SERVER_NUMS_PER_ARRAY);
	pNewClientCountsList->pNext = NULL;
		
		
	Simple_List<Server_Address_Info*>* pNewServerInfoList = new Simple_List<Server_Address_Info*>;
	pNewServerInfoList->Data = new Server_Address_Info[MAX_SERVER_NUMS_PER_ARRAY];
	pNewServerInfoList->pNext = NULL;
	
	if (0 == iListIndex)
	{
		g_pClientCountsList[m_iThreadIndex] = pNewClientCountsList;
		g_pServerInfoList[m_iThreadIndex] = pNewServerInfoList;
	}
	else
	{
		Simple_List<long int*>* pClientCountsList = g_pClientCountsList[m_iThreadIndex];
		Simple_List<Server_Address_Info*>* pServerInfoList = g_pServerInfoList[m_iThreadIndex];
		
		int i = 1;
		while (i < iListIndex)
		{
			pClientCountsList = pClientCountsList->pNext;
			pServerInfoList = pServerInfoList->pNext;
			++i;
		}
		
		pClientCountsList->pNext = pNewClientCountsList;
		pServerInfoList->pNext = pNewServerInfoList;
	}
}

// Add a new server to the server list, which other threads access by read operations
void CLoadBalancer::AddNewServer(Server_Data_Access_Info* pServerInfo_, unsigned char* pRecvBuff_)
{
	// Calculate Indicies
	unsigned long uiServerCounts = g_uiServerCounts[m_iThreadIndex];
	int iArrIndex = uiServerCounts % MAX_SERVER_NUMS_PER_ARRAY;
	int iListIndex = uiServerCounts / MAX_SERVER_NUMS_PER_ARRAY;
			
	pServerInfo_->iArrayIndex = iArrIndex;
	pServerInfo_->iListIndex = iListIndex;
	
	// Allocate memory only when the array is out of space
	if (0 == iArrIndex && 0 != iListIndex)
		AllocateMemoryForNewServers();

	Simple_List<long int*>* pClientCountsList = g_pClientCountsList[m_iThreadIndex];
	Simple_List<Server_Address_Info*>* pServerInfoList = g_pServerInfoList[m_iThreadIndex];
	int i = 0;
	while (i < iListIndex)
	{
		pClientCountsList = pClientCountsList->pNext;
		pServerInfoList = pServerInfoList->pNext;
		++i;
	}
	
	// Set the Server status Not Ready
	// The server becomes ready when this loadblaner receives the first status update packet from the server	
	pClientCountsList->Data[iArrIndex] = SERVER_NOT_READY;
	
	unsigned short int* pPort = (unsigned short int*)pRecvBuff_;
	pServerInfoList->Data[iArrIndex].usPort = *pPort;
	pServerInfoList->Data[iArrIndex].uiIP = pServerInfo_->uiIP;

	++g_uiServerCounts[m_iThreadIndex];
}

// Add a partial TCP packet to the receive queue in order to receive the rest of the packet later from where it left off
void CLoadBalancer::AddTCPPacketToRecvQueue(int iSockFD_, int iPacketType_, size_t uiBufferLength_, size_t uiOffset_, unsigned char* pRecvBuff_)
{
	InComplete_Packet* pInCompletePacket = new InComplete_Packet;
	pInCompletePacket->iPacketType = iPacketType_;
	pInCompletePacket->uiBufferLen = uiBufferLength_;
	pInCompletePacket->uiOffset = uiOffset_;
	pInCompletePacket->pBuffer = new unsigned char[uiBufferLength_];
	memcpy((void*)pInCompletePacket->pBuffer, (void *)pRecvBuff_, uiOffset_);
	
	m_mapTCPPacketRecvQueue.insert(std::make_pair(iSockFD_, pInCompletePacket));
}

// Erase a packet from the receive queue and release memory
void CLoadBalancer::RemoveTCPRecvQueuePacket(int iSockFD_, InComplete_Packet* pInCompletePacket_)
{
	delete pInCompletePacket_->pBuffer;
	delete pInCompletePacket_;
	m_mapTCPPacketRecvQueue.erase(iSockFD_);
}

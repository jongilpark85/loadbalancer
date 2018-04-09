#pragma once
#include <iostream>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h> 
#include <list>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <limits.h>
#include <deque>
#include "Common_Header.h"

// The Number of Threads (Including the main thread)
#define MAX_THREAD_COUNTS 4

// For EPoll
// Up to MAX_EVENT_COUNTS are returned by epoll_wait()
#define MAX_EVENT_COUNTS 256

// UDP uses a socket to communicate with multiple clients.
// If the load balancer processes only one packet in the socket bufffer on an EPOLLIN event,
// the load balancer will unecessarily call epoll_wait too many times.
// If the load balancer tries to process all the packets in the socket buffer,
// packets from servers may not be procssed on time.
// Thus, it is neccesary to have a limit on the number of UDP packets to process at a time.
// For testing, the value is set to 1
#define MAX_UDP_PACKET_LOOPING_COUNT 1

// Same reasoning as UDP packet receive
// The maximum number of client connections the load balancer accepts in a loop
// For testing, the value is set to 1
#define MAX_CLIENT_ACCEPT_LOOPING_COUNT 1

// Same reasoning as UDP packet receive
// The maximum number of server connections the load balancer accepts in a loop
// For testing, the value is set to 1
#define MAX_SERVER_ACCEPT_LOOPING_COUNT 1

// Server Status
#define SERVER_NOT_READY		-1
#define SERVER_DISCONNECTED		-2
#define SERVER_NEVER_CONNECTED	-3

// Frequent memory allocation could increase overhead, so memory for MAX_SERVER_NUMS_PER_ARRAY (20) servers are allocated at once.
#define MAX_SERVER_NUMS_PER_ARRAY	20

// Information of the address of a server
struct Server_Address_Info
{
	in_addr_t uiIP;
	unsigned short usPort;
};

// Information to access data of a server
struct Server_Data_Access_Info
{
	int iSocketFD;
	int iListIndex;
	int iArrayIndex;
	in_addr_t uiIP;
};

// Types of packets from servers for internal use
enum SERVER_PACKET_TYPE
{
	SPT_PORT = 0,
	SPT_STATUS = 1,
	SPT_MAX,
};

// struct for a simple list
template <typename T>
struct Simple_List
{
	T Data;
	Simple_List* pNext;
};



// We need to hanlde partial data transmission when we use TCP.
// With a stream-oriented socket, it is possible that a packet may arrive in an incomplete format.(Partial data transmission)
// For example, assume that a packet is expected to be 400 bytes long, but only 300 bytes has been transferred.
// When we use epoll, we will be notified when a socket is readable.
// Thus, our first recv() call shall return 300 bytes, and we can assume that there is no more data in the socket buffer at this moment.
// We could keep calling recv() in a loop until we receive 400 bytes, but this is very inefficient.
// We need to store the partial data (300 bytes) somewhere, and call recv() when epoll notifies us again that the socket is readable.
// Then, we combine the previous data (300 bytes) with the newly recevied data. 
// If the newly received data is 100 bytes long, we can perform some actions corresponding to the packet.
// If the newly received data is smaller thatn 100 bytes, we need to store it and come back later.
// Partial data transmission could occur in send() as well.
// If space is not fully available for a packet to be transmitted, the rest of the packet also needs to be stored until space is availabe.
// For this load balancer, such situation is not likely to happen because even the largest packet is about 20 bytes long.

// This struct is for resolving partial data transmission issue with TCP
struct InComplete_Packet
{ 
	// Buffer Length
	size_t uiBufferLen;
	
	// The number of bytes that has already been received or sent.
	size_t uiOffset;
	
	// If -1, pBuffer contains part of packet header
	// Otherwise, pBuffer contains part of packet data
	// This is only for recv
	int iPacketType;
	
	// The buffer that contains a partial packet
	unsigned char* pBuffer;
	
};


// For sendto() with UDP,
// With UDP, the entire message shall be read or written in a single operation, so there's no need to worry about partial packet transmission.
// Thus, even if recvfrom() fails, the load balancer can simply come back later without storing any data. 
// However, it is possible that sendto() will fail if space is not available for a packet to be transmitted at the moment of a sendto() call.
// The packet needs to be stored until space is available.
// This guarantees that the packet will be sent out, but does not guarantee that the packet will be delivered.
struct Queued_UDP_Packet
{
	// Destination Address
	sockaddr_in stSockAddr;
	
	// Size of Destination Address 
	socklen_t uiAddrLen;
	
	// Buffer Length
	size_t uiBufferLen;
	
	// The buffer that contains a UDP Packet
	unsigned char* pBuffer;
};



// Class For the Load Balancer
class CLoadBalancer
{
public:
	CLoadBalancer(__uint16_t uiPort1_, __uint16_t uiPort2_, int iThreadIndex_); // Constructor
	~CLoadBalancer(); // Destructor
	
	int SetUp(); // Set up sockets to accept incoming connections and packets
	void Run(); // Main loop that handles epoll events and manages communication with servers and clients
	void DisplayErrorMessage(const char* szErrorMessage_); // Print out an error message
	
private:
	// For communication with Clients
	int m_iListenSockForClients; // TCP listening socket to communcate with clients
	int m_iUDPSockForClients; //  UDP socket to communicate with clients
	unsigned short m_usPortForClients; // Port clients
	
	// For communication with Servers
	int m_iListenSockForServers; // TCP listening socket to communcate with servers
	unsigned short m_uiPortForServers; // Port for servers 

	int m_iEPollFD; // File descriptor referring to epoll instance
	
	// key is a socket file descriptor, and value is a pointer to information of the corresponding server
	std::unordered_map<int, Server_Data_Access_Info*> m_mapServerList; // Server list that each thread manages
	
	// For handling a situation where only part of a packet is received
	// Data received later are added to the previously received data so that they become a complete packet.
	// m_mapTCPPacketRecvQueue is a Queue for TCP Packets that were not received completely at the time of a recv call
	// Key is a socket file descriptor, and value is a pointer to information about partially received data.
	std::unordered_map<int, InComplete_Packet*> m_mapTCPPacketRecvQueue; 
	
	// For handling a situation where only part of a packet is sent
	// Packets in the queue are sent when space becomes available.
	// m_mapTCPPacketSendQueue is a Queue for TCP Packets that were not sent completely because space was not available at the time of a send call
	// Key is a socket file descriptor, and value is a pointer to information about pending data.
	std::unordered_map<int, InComplete_Packet*> m_mapTCPPacketSendQueue; 
	
	// Queue for UDP Packets that were not transferred because space was not available at the time of a sendto call
	std::list<Queued_UDP_Packet*> m_listUDPPacketQueue;

	int m_iThreadIndex; // Each thread is assigned an index to access the corresponing elements of arrays shared among all the threads
	
	size_t m_uiPacketDataLength[SPT_MAX]; // The length of the data section of a packet for each packet type


private:
	// Create a TCP listening socket and set it up to accept incomming connections.
	int SetUpTCPListenSocket(unsigned short usPort_); 
	
	// Create a UDP socket and set it up to communicate with clients
	int SetUpUDPSocket(unsigned short usPort_);
	
	// Set up socket to be ready for communication with clients and servers
	int SetUpSocket(int iSockFD_, unsigned short uiPort_, uint32_t uiEpollEvents_); 
	
	// Enable socket options
	int SetSocketOptions(int iSockFD_); 
	
	// Make the sock use Non-blocking mode
	int SetNonBlocking( int iSockFD_); 
	
	// Handle an EPOLLIN event
	int EpollInEventHandler(int iSockFD_); 
	
	// Handle an EPOLLOUT event
	int EpollOutEventHanlder(int iSockFD_); 
	
	// Handle a disconnected client or server
	int DisconnectHandler(int iSockFD_); 
	
	// Remove a server from the list when the server gets disconnected
	void RemoveServer(int iSockFD_); 
	
	// Build a response that will be sent to the Client
	void BuildResponse(unsigned char* szRecBuff_, unsigned char* szSendBuff__); 

	// Choose the server with the fewest clients among all the servers
	void GetBestServer(int* pThreadIndex_, int* pListIndex_, int* pArrIndex_); 
	
	// Get IP and Port of the Server corresponding to the indices
	void GetServerAddr(unsigned char* pBuff_, int iThreadIndex_, int iListIndex_, int iArrIndex_); 
	
	// Get the type of a packet
	int GetPacketType(unsigned char* pRecvBuff_);
	
	// Get the length of the data section of a packet
	size_t GetPacketDataLength(int iPacketType_);
	
	// Receive data from a server (TCP)
	int ServerPacketHandler(Server_Data_Access_Info* pServerInfo_);
	
	// Receive data from a server when there is no previous data partially received (TCP)
	int RecvServerPacket(Server_Data_Access_Info* pServerInfo_);
	
	// Receive data from a server and add it to the previous data partially received (TCP)
	int RecvServerPacketWithPreData(Server_Data_Access_Info* pServerInfo_, InComplete_Packet* pInCompletePacket_);
	
	// Receive a UDP packet from a client 
	int ClientUDPPacketHandler(int iSockFD_);
	
	// Receive data from a client (TCP)
	int ClientTCPPacketHandler(int iSockFD_);
	
	// Receive data from a client when there is no previous data partially received (TCP)
	int RecvClientPacket(int iSockFD_);
	
	// Receive data from a client and add it to the previous data partially received (TCP)
	int RecvClientPacketWithPreData(int iSockFD_, InComplete_Packet* pInCompletePacket_);
	
	// Send the IP and Port of the least busy server to the client
	int SendResponseToClient(int iSockFD_, unsigned char* pRecvBuff_);
	
	// Send a TCP packet in the queue,  which contains TCP packets that were sent out partially
	int SendTCPQueuePacket(int iSockFD_);
	
	// Send all of th UDP packets in the queue until space is not available or queue is empty
	int SendUDPQueuePacket(int iSockFD_);
	
	// Add a new server to the server list, which other threads access by read operations
	void AddNewServer(Server_Data_Access_Info* pServerInfo_, unsigned char* pRecvBuff_);
	
	// Accept an incoming connection and register the socket to the epoll descriptor
	int AcceptConnection(int iListenSockFD_, sockaddr_in* pSockAddr_, socklen_t* pAddrLen_);
	
	// Update the status of a server with the new value transferred from that server
	void UpdateServerStatus(Server_Data_Access_Info* pServerInfo_, unsigned char* pReceivedData_);

	// Wrapper for epoll_ctl() 
	int Epoll_CTL_Wrapper(int iOption_, int iSockFD_, unsigned int uiEvent_);
	
	// Add a partial TCP packet to the receive queue in order to receive the rest of the packet later from where it left off
	void AddTCPPacketToRecvQueue(int iSockFD_, int iPacketType_, size_t uiBufferLength_, size_t uiOffset_, unsigned char* pRecvBuff_);
	
	// Erase a TCP packet from the receive queue and release memory
	void RemoveTCPRecvQueuePacket(int iSockFD_, InComplete_Packet* pInCompletePacket_);
	
	// Add a partial TCP packet to the send queue in order to send the rest of the packet when space is availabe
	void AddTCPPacketToSendQueue(int iSocket_, unsigned char* pSendBuff_, size_t uiRemainBytes_); 
	
	// Erase a TCP packet from the send queue and release memory
	void RemoveTCPSendQueuePacket(int iSockFD_, InComplete_Packet* pPacket_); //
	
	// Allocate memory to store information about the new servers
	void AllocateMemoryForNewServers(); 
};


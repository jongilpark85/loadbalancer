#pragma once

// Packet Structure
// from Server to Load Balancer
// 1. Port packet: packet type (unsigned short) + port number(unsigned short) 
// 2. Status packet: packet type (unsigned short) + the number of connected clients (long int)

// from Client to Load Balancer
// 1. Server Address Request Packet: packet type (unsigned short)

// from Load Balancer to Client
// 1. Server Address Response Packet: packet type (unsigned short) + error code(unsigned short) + server port(unsigned short) + server IP(in_addr_t)
// Error Code
// 1. Sucess 
// 2. There is no running server 
// 3. Wrong packet type was sent by the client 
// Instead of Error Coode, more packet types could have been used( Either way has its own pros and cons)


// As seen above, packet size varies depending on a packet type or error code.
// A fixed sized buffer or a separte packet header could be used to handle various sized packets.
// To demonstrate this, the load balancer uses a fixed sized buffer on communicating with clients.
// When communicating with servers, the load balancer first receives the packet type as a packet header, get the packet size from it, receives the data section of the packet. 


// Between Servers and Load Balancer
// This is the size of the header of a packet because the header only contains the type value in current implementation
#define PACKET_TYPE_LENGTH 2

// Server Port Number Packet
#define SERVER_PORT_NUM_PACKET_TYPE 10000 /// an arbirary value to indicate that the packet is Server Port Number type
#define SERVER_PORT_NUM_PACKET_DATA_LENGTH 2 // The length of the data section of Server Port Number Packet

// Server Status Update Packet
#define SERVER_STATUS_UPDATE_PACKET_TYPE 20000 // an arbirary value to indicate that the packet is Server Status Update type
#define SERVER_STATUS_UPDATE_PACKET_DATA_LENGTH 8 // The length of the data section of Server U Packet


// Between Clients and Load Balancer
// This length should be the length of the largest packet because a fixed sized buffer is used
// Now, there is only one type of packet from client to load balancer.
#define REQUEST_FROM_CLIENT_LENGTH	2

// This length should be the length of the largest packet because a fixed sized buffer is used
// Now, there are three types of packet from load balancer to client.
// The largest one is 10 bytes long
#define RESPONSE_TO_CLIENT_LENGTH	10

// Server Address Request Packet
#define SERVER_ADDR_REQUEST_TYPE 10000 // an arbirary value

// Response Type for Sever Address Request Packet
#define SERVER_ADDR_RESPONSE_SUCCESS 0
#define SERVER_ADDR_RESPONSE_NO_SERVER 1
#define SERVER_ADDR_RESPONSE_UNKNOWN_TYPE 2

// The load balancer take command line arguments to specify its open ports
// If no arguments are provided, then use the default port numbers ( For testing)
#define LB_PORT_FOR_SERVER 43000
#define LB_PORT_FOR_CLIENT 53000 

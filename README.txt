1. Overview
    This load balancer is completely lock-free and supports both TCP and UDP clients.
    Ths load balancer uses TCP with servers, and each server repeatedly sends its status information to the load balancer on a regular time basis.
    In the current implementation, the number of clients connected to each server is used to represent how busy each server is.
    Ohter informaton, such as the number of requests that each server has received from clients for a certain time period, can be used as well.
    When a client asks the load balancer for the IP and port of a server, the load balancer chooses the server with the fewest clients connected.

    Every thread is listening on the same port by using SO_REUSEPORT option, and all the incoming connections are distributed pretty much evenly by the kernel.
    If clients use UDP, all the incoming packets are distributed by kernel among all the threads as well.
    With SO_REUSEPORT option, each thread only communicates with its own list of servers and maintains information of those servers.
    When a thread is created, it is assigned a unique index, which guarantees only one thread writes on the corresponding emelment of global arrays.
    When a thread accesses other elements of global arrays, it only performs read operations.
    In other words, each thread only writes new data on its own list and accesses other threads' list only with read operations

    It is possible that while a thread is reading data from another thread's list, the other thread is writting new data on that list.
    However, the reader thread will always get either the old value or new value, not something else.
    This is because reading or writing on natively aligned memory is done atomically.
    Each server's status, i.e. The number of clients connected to each server, is stored as an long integer.
    Thus, updating each server's status with a new value does not cause the reader thread to get any other value than the old or new value.
    Actually, the load balancer stores each server's status in an array because there are multiple servers.
    Thus, g_iClientCounts[iServerIndex] = iNewCounts may not be a single atomic operation, but the actual writing on that element is done atomically.
    Similarly, ++g_uiServerCounts is not a single atomic operation, but when a thread read g_uiServerCounts, that thread will always get either the old value or new value.
    Even if a lock were used, the result would be the same. 
    If the writer thread entered the critical section first, then, the reader thread would get the new value.
    If the reader thread entered the critical section first, then, the reader thread would get the old value.
    Therefore, the load balancer does not use any lock.
    
    Moreover, the load balancer takes care of partial data transmission that could happen on using TCP.
    With a stream-oriented socket, a packet may arrive in an incomplete format.
    For example, assume that a packet is expected to be 400 bytes long, but only 300 bytes has been transferred.
    Since some data is in the socket buffer, epoll will notify that the socket is readable.
    A recv() call shall return 300 bytes, and then, the load balaner stores the partial data (300 bytes) in a queue.
    When epoll notifies again that the socket is readable, the load blancer combines the previous data (300 bytes) with the newly received data. 
    If the newly received data is 100 bytes long, the load balancer performs some actions corresponding to the packet.
    If the newly received data is smaller thatn 100 bytes, the load balancer stores it and comes back later.
    The load balancer could keep calling recv() in a loop until it receives the rest 100 bytes, but that is very inefficient.
    Partial data transmission could occur in send() as well.
    If space is not fully available for a packet to be transmitted, the rest of the packet is also sotred in a queue until space is availabe.
    For this load balancer, such situation is not likely to happen because even the largest packet is about 20 bytes long.

    With UDP, the entire message shall be read or written in a single operation, so there's no issue with partial packet transmission.
    Thus, when recvfrom() fails, the load balancer simply comes back later without storing any data. 
    However, when sendto() fails because space is not available for a packet to be transmitted, the load balancer stores the entire UDP packet in a queue.
    UDP packets in the queue are sent when space is available.
    The load balancer guarantees that every UDP packet is sent out, but does not provide guaranteed packet delivery.  


2. Future work
    The result of the test program, test.py, may seem incorrect, but that is actually expected.
    The result of load balancing depends on two main factors.
    The first factor is how frequently each server sends a status update packet to the load balancer.
    The second factor is when a client send a server address request packet to the load balancer.
    Assume that there are two servers, server 1 and server 2, and that each server sends a status update packet every 10 seconds.
    Also, assume that server 1 has one client connected to it, and server 1 has no client.
    If 10 clients send a server address request to the load balancer between the previous status update and neext status update,
    the load balancer will give the address of server 2 to all of the clients
    This is because the load balancer still considers server 2 has no client.
    If the update time interval is short, this problem could be minimized, but there still needs a better solution.
    

3. System Requirements
    Linux (64 bit)
    Kernel Version 3.9 or higher (for SO_REUSEPORT option)
    c++11 (for std::unordered_map)


4. Complie
    You can use one of the following commands on a terminal
    $ make
    $ make build
    $ make rebuild


5. Remove object files and executables
    $ make clean


6. Automated Test
    You can use one of the following commands on a terminal
    $ make test
    $ python test.py

    The automated test takes minutes


7. Manual Test (After compilation)
    1) Run the load balancer on a terminal
        $ ./loadbalancer

    2) Run each sserver on a different terminal ( 3 servers need 3 terminals )
        $ ./server [Server Port]

        Each server should use a different port number
    
    3) Run a client on another terminal
        $ ./udp_client
        or
        $ ./tcp_client

        For testing, a client terminates after it makes a connection with a server.
        The server considers that client is still connected

    4) If 10 clients are needed, repeat step 3) 10 times on the same terminal

    5) Kill the load balancer
        $ pidof loadbalancer
        $ kill -9 [PID of loadbalancer]

    6) Check the output from each server on each terminal
        Each server will display the number of clients connected to it and terminate automatically


8. Usage (If no argument is given, pre-defined default values are used)
    1) Load balancer
        $ ./loadbalancer [port1] [port2]

        port1 is the port number on which the load balancer is listening to accept connections or receives packets from clients
        port2 is the port number on which the load balancer is listening to accept connections from servers

    2) Server
        $ ./server [port1] [ip] [port2]

        port1 is the port number on which the server is listening to accept connections from clients
        ip is the IP address of the load balancer
        port2 is the port number of the load balancer

    3) UDP Client
        $ ./udp_client [ip] [port]

        ip is the IP address of the load balancer
        port is the port number of the load balancer

    4) TCP Client
        $ ./tcp_client [ip] [port]

        ip is the IP address of the load balancer
        port is the port number of the load balancer
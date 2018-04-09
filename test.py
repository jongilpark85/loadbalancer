import subprocess
import signal
import time
import random

# Total number of servers
SERVER_NUMS = 10

# Total number of clients
CLIENT_NUMS = 500

# At least CLIENT_MIN_CREATION_AT_ONCE clients are created every time interval
CLIENT_MIN_CREATION_AT_ONCE = 20

# At most CLIENT_MIN_CREATION_AT_ONCE clients are created every time interval
CLIENT_MAX_CREATION_AT_ONCE = 40

# Every CLIENT_CREATION_TIME_INTERVAL, a random number of clients are created
CLIENT_CREATION_TIME_INTERVAL = 2

# The first server's port number
SERVER_PORT = 40000

def Run():
    loadbalancer = subprocess.Popen(["./loadbalancer"])

    print "Load Balancer has been created"
    # Wait until Load balancer gets ready
    time.sleep(5)

    i = 0
    while i < SERVER_NUMS:
        server = subprocess.Popen(["./server", str(SERVER_PORT + i) ])
        i = i + 1

    print ("%d Servers have been created" % SERVER_NUMS)

    # Wait until servers get ready
    time.sleep(5)
    count = 0
    client = None
    while count < CLIENT_NUMS:
        n = random.randint(CLIENT_MIN_CREATION_AT_ONCE, CLIENT_MAX_CREATION_AT_ONCE)
        if n > CLIENT_NUMS - count:
            n = CLIENT_NUMS - count

        j = 0
        while j < n:
            client = subprocess.Popen(["./udp_client"])
            j = j + 1

        count  = count  + n
        time.sleep(CLIENT_CREATION_TIME_INTERVAL)

    print ("%d Clients have been created" % CLIENT_NUMS)

    # Wait until the last client is connected to a server
    time.sleep(5)

    # Killing the load balancer causes servers to terminate ( For testing )
    # This is to test the functionality of the load balancer, not that of server or client.
    # For simplicity and clear result,
    # Clients terminate right after they make a connection with a server.
    # Servers still consider disconnected clients connected
    # Servers should keep running in real environment.
    loadbalancer.kill()

    print "Load Balancer has been killed"

    time.sleep(20)


if __name__ == "__main__":
    Run()
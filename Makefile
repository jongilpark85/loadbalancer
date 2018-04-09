# Makefile for Load Balancer
CXX=g++
CXXFLAGS=-std=c++11 -g -Wall

default: build

build: loadbalancer tcp_client udp_client server

rebuild: clean build
  
clean:
	rm -rf *.o loadbalancer tcp_client udp_client server

loadbalancer: LoadBalancer.o CLoadBalancer.o
	$(CXX) $(CXXFLAGS) -o loadbalancer LoadBalancer.o CLoadBalancer.o -lpthread

LoadBalancer.o: LoadBalancer.cpp CLoadBalancer.h Common_Header.h
	$(CXX) $(CXXFLAGS) -c LoadBalancer.cpp

CLoadBalancer.o: CLoadBalancer.cpp CLoadBalancer.h Common_Header.h
	$(CXX) $(CXXFLAGS) -c CLoadBalancer.cpp

tcp_client: TCP_Client.o
	$(CXX) $(CXXFLAGS) -o tcp_client TCP_Client.o

TCP_Client.o: TCP_Client.cpp Common_Header.h
	$(CXX) $(CXXFLAGS) -c TCP_Client.cpp

udp_client: UDP_Client.o
	$(CXX) $(CXXFLAGS) -o udp_client UDP_Client.o

UDP_Client.o: UDP_Client.cpp Common_Header.h
	$(CXX) $(CXXFLAGS) -c UDP_Client.cpp

server: Server.o
	$(CXX) $(CXXFLAGS) -o server Server.o -lpthread

Server.o: Server.cpp Common_Header.h
	$(CXX) $(CXXFLAGS) -c Server.cpp

test: loadbalancer tcp_client udp_client server
	python test.py



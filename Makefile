#------------------------------------------------------------------------------
# Makefile for httpserver.c
#------------------------------------------------------------------------------
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wshadow -O2 -pthread
all : loadbalancer

loadbalancer : loadbalancer.o
	gcc $(CFLAGS) -o loadbalancer loadbalancer.o

loadbalancer.o : 
	gcc $(CFLAGS) -c loadbalancer.c 

clean :
	rm -f loadbalancer.o

spotless : clean
	rm -f loadbalancer

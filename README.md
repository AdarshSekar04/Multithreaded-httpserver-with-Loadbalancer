CSE 130 asgn3
Adarsh Sekar
adsekar@ucsc.edu
1619894

Note:
This program will not work on unix, as there are a few methods that only work on linux.
To get it to work on UNIX, you must remove the define POSIX_C_SOURCE 200809L line and define GNU_SOURCE line
You may also have to change some of the ways I use getopt, as some things I do are specific to Linux.

This README is for the multithreaded loadbalancer I designed as part of my Principles of Computer System Deisgn (CSE130) that I took at UCSC. 
For more details on the httpserver, please look at my multithreaded httpserver: https://github.com/AdarshSekar04/Multithreaded-httpserver

The file should have a Makefile, loadbalancer.c to run. To compile simply call make or make all. To run the program, run ./httpserver with a port number, and an optional -N and -l header.
The -N header must be followed by an positive integer number of threads, and the -R header an integer value greater than 0. 
As of the time this README is written, the program seems to work fine.
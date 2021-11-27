all: httpserver

httpserver: main.cpp threadpool.h http_conn.cpp http_conn.h locker.h common.h
	g++ -g -std=c++11 main.cpp common.h threadpool.h http_conn.cpp http_conn.h locker.h -o httpserver -lpthread

clean:
		rm -f httpserver

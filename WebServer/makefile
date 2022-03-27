#server: main.cpp WebServer.cpp ./Utils/Utils.cpp ./Http/HttpConn.cpp ./Timer/Timer.cpp
#	$(CC) main.cpp WebServer.cpp ./Utils/Utils.cpp ./Http/HttpConn.cpp ./Timer/Timer.cpp  -lpthread -g -o server

CC = g++
CFLAGS = -Wall -g

server: main.o WebServer.o Utils.o HttpConn.o Timer.o
	$(CC) $(CFLAGS) *.o -lpthread -o server

main.o: main.cpp	
	$(CC) $(CFLAGS) -c main.cpp

WebServer.o: ./WebServer/WebServer.cpp
	$(CC) $(CFLAGS) -c ./WebServer/WebServer.cpp

Utils.o: ./Utils/Utils.cpp
	$(CC) $(CFLAGS) -c ./Utils/Utils.cpp

HttpConn.o: ./Http/HttpConn.cpp
	$(CC) $(CFLAGS) -c ./Http/HttpConn.cpp

Timer.o: ./Timer/Timer.cpp
	$(CC) $(CFLAGS) -c ./Timer/Timer.cpp

clean:
	rm -f *.o  

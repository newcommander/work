CC=g++

WORKROOT=..
THIRD=$(WORKROOT)/thirdparty/output

TARGET=hash

LIBEVENT=$(THIRD)/libevent
JSONCPP=$(THIRD)/jsoncpp
OPENSSL=$(THIRD)/openssl
MYLOG=$(THIRD)/mylog

INCLUDES=-I$(LIBEVENT)/include \
		 -I$(JSONCPP)/include \
		 -I$(OPENSSL)/include \
		 -I$(MYLOG)/include

LIBS=-L$(LIBEVENT)/lib \
	 -L$(JSONCPP)/lib \
	 -L$(OPENSSL)/lib \
	 -L$(MYLOG)/lib

LDFLAGS=-Wl,--dn -levent -lmylog -pthread -lcrypto -ljsoncpp \
		-Wl,--dy -lrt

CFLAGS=-fPIC -g -finline-functions -Wall -Werror

.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf *.o $(TARGET) log

hash: hash.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

%.o	: %.cpp
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

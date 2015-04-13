CC=g++

WORKROOT=..
THIRD=$(WORKROOT)/thirdparty/output

TARGET=hash

LIBEVENT=$(THIRD)/libevent
OPENSSL=$(THIRD)/openssl
MYLOG=$(THIRD)/mylog

INCLUDES=-I$(LIBEVENT)/include \
		 -I$(OPENSSL)/include \
		 -I$(MYLOG)/include

LIBS=-L$(LIBEVENT)/lib \
	 -L$(OPENSSL)/lib \
	 -L$(MYLOG)/lib

LDFLAGS=-Wl,--dn -levent -lmylog -pthread -lcrypto\
		-Wl,--dy -lrt

CFLAGS=-fPIC -g -finline-functions -Wall -Werror

.PHONY: all clean

all: $(TARGET)

clean:
	rm -rf *.o $(TARGET)

hash: hash.o
	$(CC) $(LIBS) $^ $(LDFLAGS) -o $@

%.o	: %.cpp
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@

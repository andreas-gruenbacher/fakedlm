CFLAGS = -g -Wall -O0

all: fakedlm lockspace dlmtest

fakedlm: fakedlm.o common.o addr.o modprobe.o crc.o
fakedlm: LDFLAGS+=-lrt

lockspace: lockspace.o common.o

dlmtest: dlmtest.o
dlmtest: LDFLAGS+=-ldlm
dlmtest.o: CFLAGS+=-D_REENTRANT

clean:
	rm -f *.o fakedlm lockspace dlmtest

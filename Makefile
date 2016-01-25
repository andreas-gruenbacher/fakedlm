CFLAGS = -g -Wall -O0

all: dlmtest

dlmtest: dlmtest.o
dlmtest: LDFLAGS+=-ldlm
dlmtest.o: CFLAGS+=-D_REENTRANT

clean:
	rm -f *.o dlmtest

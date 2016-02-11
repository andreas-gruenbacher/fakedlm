CFLAGS = -g -Wall -O0
OUTPUT_OPTION=-MMD -MP -o $@

.PHONY: all clean

all: fakedlm lockspace dlmtest

-include $(wildcard *.d)

fakedlm: fakedlm.o common.o addr.o modprobe.o crc.o
fakedlm: LDFLAGS+=-lrt

lockspace: lockspace.o common.o

dlmtest: dlmtest.o
dlmtest: LDFLAGS+=-ldlm
dlmtest.o: CFLAGS+=-D_REENTRANT

clean:
	rm -f *.o fakedlm lockspace dlmtest $(wildcard *.d)

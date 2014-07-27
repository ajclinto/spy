CC = g++

CFLAGS = -g -O3 -std=c++0x

LDFLAGS += -lncurses -lreadline

# For -static (busted)
#LDFLAGS += -lncurses -lreadline -ltinfo -lgpm

all: spy
	

srcs = \
	   spy.cpp

objs = $(srcs:.cpp=.o)

%.o: %.cpp Makefile
	$(CC) $(CFLAGS) -c $<

spy: $(objs)
	$(CC) $(CFLAGS) $(objs) $(LDFLAGS) -o spy

clean:
	rm -f *.o *.d spy


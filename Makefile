# Intel compiler options
#CC = icc
#CFLAGS = -O3 -ipo -funroll-loops -fomit-frame-pointer

# GCC compiler options

CC = g++

CFLAGS = -O3 -g
CFLAGS += -I/usr/include

#LDFLAGS += -lpthread -ldl -lm
LDFLAGS += -lncurses -lreadline

all: spy
	

srcs = \
	   spy.cpp

objs = $(srcs:.cpp=.o)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $<

spy: $(objs)
	$(CC) $(CFLAGS) $(objs) $(LDFLAGS) -o spy

clean:
	rm -f *.o *.d spy


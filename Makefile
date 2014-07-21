# Intel compiler options
#CC = icc
#CFLAGS = -O3 -ipo -funroll-loops -fomit-frame-pointer

# GCC compiler options

CC = g++

CFLAGS = -O3
CFLAGS += -I/usr/include

#LDFLAGS += -lpthread -ldl -lm
LDFLAGS += -lncurses -lreadline

all: spy
	

srcs = \
	   spy.cpp

objs = $(srcs:.cpp=.o)

spy: $(objs)
	$(CC) $(CFLAGS) $(srcs) $(LDFLAGS) -o spy

clean:
	rm *.o *.d spy


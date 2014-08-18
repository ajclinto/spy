CC = g++

CFLAGS = -O3 -std=c++0x
LDFLAGS = -lncurses -lreadline

# For -static (busted)
#LDFLAGS += -lncurses -lreadline -ltinfo -lgpm

all: spy
	

srcs = \
	   spy.cpp

spyrc_defaults.h: spyrc_defaults
	xxd -i $< $@

spy.o: spyrc_defaults.h

objs = $(srcs:.cpp=.o)

%.o: %.cpp Makefile
	$(CC) $(CFLAGS) -c $<

spy: $(objs)
	$(CC) $(CFLAGS) $(objs) $(LDFLAGS) -o spy

clean:
	rm -f *.o *.d spyrc_defaults.h spy

# Custom built ncurses
# ./configure --with-shared --without-normal --without-debug --enable-sigwinch
# ./configure --enable-sigwinch

#CFLAGS = -Incurses/ncurses-5.9/include -g -std=c++0x
#LDFLAGS = -Lncurses/ncurses-5.9/lib -lncurses_g -lreadline

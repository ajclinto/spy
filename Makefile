CXX = g++

CFLAGS = -O3 -std=c++0x
LDFLAGS = -lncurses -ltinfo -lreadline -lrt

all: spy
	

srcs = \
	   spy.cpp

spyrc_defaults.h: spyrc_defaults
	xxd -i $< $@

spy.o: spyrc_defaults.h

objs = $(srcs:.cpp=.o)

%.o: %.cpp Makefile
	$(CXX) $(CFLAGS) -c $<

spy: $(objs)
	$(CXX) $(CFLAGS) $(objs) $(LDFLAGS) -o spy

clean:
	rm -f *.o *.d spyrc_defaults.h spy

# Custom built ncurses
# ./configure --with-default-terminfo-dir=/lib/terminfo --with-shared --without-normal --without-debug
# ./configure --with-default-terminfo-dir=/lib/terminfo

#CFLAGS = -Incurses/ncurses-6.1/include -g -std=c++0x
#LDFLAGS = -Lncurses/ncurses-6.1/lib -static -lncurses_g -lreadline -ldl


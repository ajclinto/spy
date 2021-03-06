#ifndef TIMER_H
#define TIMER_H

#include <stdio.h>
#include <time.h>

class TIMER {
public:
	 TIMER(bool print = true) : myPrint(print) { start(); }
	~TIMER()
	{
		if (myPrint)
			fprintf(stderr, "%f\n", lap());
	}

	void start()
	{
		myStart = myLap = time();
	}
	double lap()
	{
		double	cur = time();
		double	val = cur - myLap;
		myLap = cur;
		return val;
	}
	double elapsed() const
	{
		return time()-myStart;
	}

private:
	double time() const
	{
		timespec	ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		return ts.tv_sec + (1e-9 * (double)ts.tv_nsec);
	}

private:
	double	myStart;
	double	myLap;
	bool	myPrint;
};

#endif

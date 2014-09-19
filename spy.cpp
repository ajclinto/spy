#include <stdlib.h>
#include <curses.h>
#include <termcap.h>
#include <termios.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fnmatch.h>
#include <wordexp.h>
#include <pwd.h>
#include <regex.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>

#include "spyrc_defaults.h"

#include "timer.h"

// Compile time parameters (could be made settings)
static const int XPADDING = 1;
static const bool RELAXCASE = true;
static const bool HLSEARCH = false;

// Environment
static const char *s_shell = getenv("SHELL");
static const char *s_home = getenv("HOME");
static const char *s_editor = getenv("EDITOR");
static const char *s_pager = getenv("PAGER");

// History
static const std::string s_chistoryfile = std::string(s_home) + "/.spy_history";
static const std::string s_jhistoryfile = std::string(s_home) + "/.spy_jumps";
static HISTORY_STATE s_jump_history;
static HISTORY_STATE s_search_history;
static HISTORY_STATE s_execute_history;

// Child process
static int thechild = 0;

// Details mode
enum DETAIL_TYPE {
	DETAIL_NONE,
	DETAIL_SIZE,
	DETAIL_TIME,
	DETAIL_MAX
};

DETAIL_TYPE thedetail = DETAIL_NONE;
static int thedetailsizewidth = 0;
static const int thedetailtimewidth = 18;

static inline int SYSmax(int a, int b) { return a > b ? a : b; }
static inline int SYSmin(int a, int b) { return a < b ? a : b; }

static inline void replaceall(std::string &str,
		const std::string& from,
		const std::string& to)
{
	size_t start_pos = 0;
	while((start_pos = str.find(from, start_pos)) != std::string::npos)
	{
		str.replace(start_pos, from.length(), to);
		start_pos += to.length();
	}
}

static inline void replaceall_non_escaped(
		std::string &str,
		char from,
		const std::string& to)
{
	size_t start_pos = 0;
	while((start_pos = str.find(from, start_pos)) != std::string::npos)
	{
		if (start_pos == 0 || str[start_pos-1] != '\\')
		{
			str.replace(start_pos, 1, to);
			start_pos += to.length();
		}
		else if (str[start_pos-1] == '\\')
		{
			str.replace(start_pos-1, 1, "");
		}
		else
		{
			start_pos++;
		}
	}
}

static void quit_prep();

static void quit()
{
	quit_prep();
	exit(0);
}

static void signal_handler(int sig)
{
	// The first SIGINT should only kill the child process, if it exists.
	if (thechild && sig == SIGINT)
	{
		kill(thechild, sig);
	}
	else
	{
		quit();
	}
}

static bool theresized = false;

static void signal_resize(int)
{
	// Tell the main event loop that the terminal was resized
	theresized = true;
}

struct ci_equal {
	bool operator()(char ch1, char ch2) {
		return std::tolower(ch1) == std::tolower(ch2);
	}
};

// find substring (case insensitive)
static int ci_find_substr(const std::string& str1, const std::string& str2)
{
	std::string::const_iterator it = std::search(
			str1.begin(), str1.end(), 
			str2.begin(), str2.end(),
			ci_equal());
	if (it != str1.end())
		return it - str1.begin();
	return -1; // not found
}

// Extract a decimal integer from the string, and leave 'a' pointing to the
// next character after the integer.
static inline int extract_integer(const char *&a)
{
	int val = *a - '0';
	++a;
	while (isdigit(*a))
	{
		val *= 10;
		val += *a - '0';
		++a;
	}
	return val;
}

class SPY_REGEX {
public:
	SPY_REGEX(const char *pattern)
	{
		m_valid = !regcomp(&m_regex, pattern, RELAXCASE ? REG_ICASE : 0);
	}
	bool search(const char *str, int &start, int &end) const
	{
		regmatch_t match;
		if (m_valid && !regexec(&m_regex, str, 1, &match, 0))
		{
			start = match.rm_so;
			end = match.rm_eo;
			return true;
		}
		return false;
	}

private:
	regex_t m_regex;
	bool m_valid;
};

class DIRINFO {
public:
	DIRINFO() : mydirectory(false) {}

	const std::string &name() const { return myname; }
	void setname(const char *name) { myname = name; }

	bool isdirectory() const { return mydirectory; }
	void setdirectory() { mydirectory = true; }

	void setdirectory_from_stat()
	{
		lazy_stat();
		mydirectory = S_ISDIR(mystat->st_mode);
	}

	bool isexecute() const { lazy_stat(); return mystat->st_mode & S_IXUSR; }
	bool iswrite() const { lazy_stat(); return mystat->st_mode & S_IWUSR; }
	bool islink() const { lazy_stat(); return (mystat->st_mode & S_IFMT) == S_IFLNK; }

	size_t size() const { lazy_stat(); return mystat->st_size; }
	time_t modtime() const { lazy_stat(); return mystat->st_mtime; }

	bool operator<(const DIRINFO &rhs) const
	{
		bool adir = isdirectory();
		bool bdir = rhs.isdirectory();
		if (adir != bdir)
			return adir > bdir;

		switch (thedetail)
		{
			case DETAIL_SIZE:
				if (size() != rhs.size())
				{
					return size() > rhs.size();
				}
				break;
			case DETAIL_TIME:
				if (modtime() != rhs.modtime())
				{
					return modtime() > rhs.modtime();
				}
				break;
		}

		// Lexicographic compare that extracts integers and compares them
		// as integers
		const char *a = myname.c_str();
		const char *b = rhs.myname.c_str();
		while (*a && *b)
		{
			char ac = *a;
			char bc = *b;

			// Faster than tolower()
			ac = (ac >= 'A' && ac <= 'Z') ? ac+32 : ac;
			bc = (bc >= 'A' && bc <= 'Z') ? bc+32 : bc;

			// Ignore leading zeros
			bool adigit = ac > '0' && ac <= '9';
			bool bdigit = bc > '0' && bc <= '9';

			if (adigit && bdigit)
			{
				int aint = extract_integer(a);
				int bint = extract_integer(b);
				if (aint != bint)
					return aint < bint;
			}
			else
			{
				if (ac != bc)
					return ac < bc;

				++a;
				++b;
			}
		}

		return *a < *b;
	}

	bool match(const SPY_REGEX *search) const
	{
		int hlstart, hlend;
		return match(search, hlstart, hlend);
	}
	bool match(const SPY_REGEX *search, int &hlstart, int &hlend) const
	{
		if (!search)
			return false;

		return search->search(myname.c_str(), hlstart, hlend);
	}

private:
	void lazy_stat() const
	{
		if (!mystat)
		{
			mystat.reset(new struct stat);

			// Use lstat rather than stat so that symbolic links are not
			// followed, and we can get information about the link itself
			lstat(myname.c_str(), mystat.get());
		}
	}

	std::string myname;
	mutable std::unique_ptr<struct stat> mystat;
	bool mydirectory;
};

static const int BUFSIZE = 1024;

// File/directory state
static std::vector<DIRINFO> thefiles;
static char thecwd[FILENAME_MAX];

// Current file/page
static int thecurfile = 0;
static int thecurpage = 0;
static int thecurcol = 0;
static int thecurrow = 0;

// Saved per-directory current file
static std::map<std::string, std::string> thesavedcurfile;

// Layout info
static int thepages = 0;
static int therows = 0;
static int thecols = 0;

// Messages
static std::string themsg;
static bool thedebugmode = false;

// Search info
static std::unique_ptr<SPY_REGEX> thesearch;

// Color info
struct COLOR {
	enum COLORTYPE {
		DIRECTORY,
		EXECUTABLE,
		READONLY,
		LINK,
		TAGGED,
		PATTERN
	};

	COLOR(const std::string &pattern, int color)
		: mycolor(color)
		{
			if (pattern == "-dir")
				mytype = DIRECTORY;
			else if (pattern == "-x")
				mytype = EXECUTABLE;
			else if (pattern == "-ro")
				mytype = READONLY;
			else if (pattern == "-link")
				mytype = LINK;
			else if (pattern == "-tagged")
				mytype = TAGGED;
			else
			{
				mypattern = pattern;
				mytype = PATTERN;
			}
		}

	std::string mypattern;
	COLORTYPE mytype;
	int mycolor;
};

static std::vector<COLOR> thecolors;

// Ignore info
struct IGNOREMASK {
	IGNOREMASK() : myenable(true) {}

	std::vector<std::string> mypatterns;
	bool myenable;
};

static std::map<std::string, IGNOREMASK> theignoremask;

static bool ignored(const char *name)
{
	for (auto it = theignoremask.begin(); it != theignoremask.end(); ++it)
	{
		const IGNOREMASK &mask = it->second;
		if (mask.myenable)
		{
			for (auto pat = mask.mypatterns.begin(); pat !=
					mask.mypatterns.end(); ++pat)
			{
				if (!fnmatch(pat->c_str(), name, FNM_PERIOD))
					return true;
			}
		}
	}
	return false;
}

static int itoawidth(size_t size)
{
	int width = 0;
	while (size)
	{
		width++;
		size /= 10;
	}
	return SYSmax(width, 1);
}

static void layout(const std::vector<DIRINFO> &dirs, int ysize, int xsize)
{
	int maxwidth = 0;
	for (int i = 0; i < dirs.size(); i++)
	{
		maxwidth = SYSmax(maxwidth, dirs[i].name().length());
	}

	maxwidth += XPADDING;
	switch (thedetail)
	{
		case DETAIL_NONE:
			maxwidth += 2;
			break;
		case DETAIL_SIZE:
			{
				size_t maxsize = 0;
				for (int i = 0; i < dirs.size(); i++)
				{
					maxsize = SYSmax(maxsize, dirs[i].size());
				}
				thedetailsizewidth = itoawidth(maxsize);
				maxwidth += thedetailsizewidth+2;
			}
			break;
		case DETAIL_TIME:
			maxwidth += thedetailtimewidth+2;
			break;
	}

	therows = SYSmax(ysize, 1);
	thecols = xsize / (maxwidth + XPADDING);
	thecols = SYSmax(thecols, 1);

	thepages = (dirs.size()-1) / (thecols * therows) + 1;
}

static void filetopage(int file, int &page, int &col, int &row)
{
	page = file / (therows*thecols);

	file %= (therows*thecols);
	col = file / therows;
	row = file % therows;
}

static void pagetofile(int &file, int page, int col, int row)
{
	file = row + therows*(col + thecols*page);
}

static void filetopage()
{ filetopage(thecurfile, thecurpage, thecurcol, thecurrow); }
static void pagetofile()
{ pagetofile(thecurfile, thecurpage, thecurcol, thecurrow); }

// Set the current file to the one matching the given name, if it exists
static void find_and_set_curfile(const std::string &name)
{
	for (int file = 0; file < thefiles.size(); file++)
	{
		if (name == thefiles[file].name())
		{
			thecurfile = file;
			filetopage();
			break;
		}
	}
}

static void rebuild()
{
	TIMER	timer(false);
	double	buildtime;
	double	sorttime;
	double	layouttime;

	// Get the directory listing
	if (!getcwd(thecwd, sizeof(thecwd)))
	{
		themsg = "Could not get current directory";
		return;
	}

	DIR *dp = opendir(thecwd);
	if (dp == NULL)
	{
		themsg = "Could not get directory listing";
		return;
	}

	// Save the current file name
	std::string prevfile;
	if (thecurfile < thefiles.size())
		prevfile = thefiles[thecurfile].name();

	thefiles.clear();

	struct dirent entry;
	struct dirent *result;
	readdir_r(dp, &entry, &result);
	while(result)
	{
		if (strcmp(entry.d_name, ".") &&
			strcmp(entry.d_name, "..") &&
			!ignored(entry.d_name))
		{
			thefiles.push_back(DIRINFO());
			thefiles.back().setname(entry.d_name);

			if (entry.d_type == DT_DIR)
			{
				thefiles.back().setdirectory();
			}
			else if (entry.d_type == DT_UNKNOWN)
			{
				thefiles.back().setdirectory_from_stat();
			}
		}
		readdir_r(dp, &entry, &result);
	}

	if (thedebugmode)
		buildtime = timer.elapsed();

	std::sort(thefiles.begin(), thefiles.end());

	if (thedebugmode)
		sorttime = timer.elapsed();

	layout(thefiles, LINES-3, COLS);

	// Restore the current file if possible. This allows reordering (eg.
	// toggling details or refreshing the directory) to preserve the
	// selection.
	if (!prevfile.empty())
		find_and_set_curfile(prevfile);

	if (thecurfile >= thefiles.size())
		thecurfile = thefiles.size() ? thefiles.size()-1 : 0;

	filetopage();

	closedir(dp);

	if (thedebugmode)
	{
		layouttime = timer.elapsed();

		char buf[BUFSIZE];
		sprintf(buf, "build time: %f sort time: %f layout time %f",
				buildtime,
				sorttime-buildtime,
				layouttime-sorttime);
		themsg = buf;
	}
}

static void set_attrs(const DIRINFO &dir, bool curfile)
{
	if (curfile)
	{
		attrset(COLOR_PAIR(0));
		attron(A_REVERSE);
	}
	else
	{
		int color = 0; // Black
		for (int i = 0; i < thecolors.size(); i++)
		{
			switch (thecolors[i].mytype)
			{
				case COLOR::DIRECTORY:
					if (dir.isdirectory())
					{
						color = thecolors[i].mycolor;
					}
					break;
				case COLOR::EXECUTABLE:
					if (!dir.isdirectory() && dir.isexecute())
					{
						color = thecolors[i].mycolor;
					}
					break;
				case COLOR::READONLY:
					if (!dir.isdirectory() && !dir.iswrite())
					{
						color = thecolors[i].mycolor;
					}
					break;
				case COLOR::LINK:
					if (dir.islink())
					{
						color = thecolors[i].mycolor;
					}
					break;
				case COLOR::TAGGED:
					break;
				case COLOR::PATTERN:
					if (!fnmatch(thecolors[i].mypattern.c_str(),
								dir.name().c_str(), FNM_PERIOD))
					{
						color = thecolors[i].mycolor;
					}
					break;
			}
		}

		attrset(COLOR_PAIR(color));
	}
}

static void puttime(const char *format, struct tm &time)
{
	char date[BUFSIZE];

	strftime(date, BUFSIZE, format, &time);
	attrset(A_NORMAL);
	addnstr(date, BUFSIZE);
}

static void putpunc(char c)
{
	attrset(COLOR_PAIR(4));
	addch(c);
}

static void drawfile(int file, const SPY_REGEX *incsearch)
{
	int page, x, y;
	filetopage(file, page, x, y);

	int xoff = (x * COLS) / thecols;

	const DIRINFO &dir = thefiles[file];

	// Draw details
	switch (thedetail)
	{
		case DETAIL_NONE:
			{
				// Draw the '*' for directories
				set_attrs(dir, false);
				if (dir.isdirectory())
				{
					move(2+y, xoff);
					addch('*');
				}
			}
			xoff += 2;
			break;
		case DETAIL_SIZE:
			{
				// Draw the file size. Blocks of 3 digits will alternate in
				// color.
				bool color = true;
				int i = 0;
				size_t s = dir.size();
				do
				{
					move(2+y, xoff+thedetailsizewidth-i-1);
					if (!(i % 3))
						color = !color;
					attrset(color ? COLOR_PAIR(4) : A_NORMAL);
					addch((s % 10) + '0');
					s /= 10;
					i++;
				} while (s);
			}
			xoff += thedetailsizewidth+2;
			break;
		case DETAIL_TIME:
			{
				// Draw the modification time
				time_t modtime = dir.modtime();
				time_t nowtime = time(0);
				time_t yestime = nowtime - 60*60*24;
				struct tm modtm;
				struct tm nowtm;
				struct tm yestm;

				localtime_r(&modtime, &modtm);
				localtime_r(&nowtime, &nowtm);
				localtime_r(&yestime, &yestm);

				move(2+y, xoff);

				if (nowtm.tm_mday == modtm.tm_mday &&
					nowtm.tm_mon == modtm.tm_mon &&
					nowtm.tm_year == modtm.tm_year)
				{
					attrset(A_NORMAL);
					addstr("    Today");
				}
				else if (yestm.tm_mday == modtm.tm_mday &&
						 yestm.tm_mon == modtm.tm_mon &&
						 yestm.tm_year == modtm.tm_year)
				{
					attrset(A_NORMAL);
					addstr("Yesterday");
				}
				else
				{
					puttime("%b %d", modtm);
					putpunc('/');
					puttime("%g", modtm);
				}

				time_t difftime = nowtime - modtime;
				if (difftime == 0)
				{
					printw("      now", (int)difftime);
				}
				else if (difftime < 60)
				{
					printw("      %03d", -(int)difftime);
				}
				else if (difftime < 3600)
				{
					attrset(A_NORMAL);
					printw("   %3d", -(int)(difftime/60));
					putpunc(':');
					attrset(A_NORMAL);
					printw("%02d", (int)(difftime%60));
				}
				else
				{
					puttime(" %k", modtm);
					putpunc(':');
					puttime("%M", modtm);
					putpunc(':');
					puttime("%S", modtm);
				}
			}
			xoff += thedetailtimewidth+2;
			break;
	}

	move(2+y, xoff);

	int maxlen = SYSmax(COLS - xoff, 0);
	int hlstart;
	int hlend;
	if (dir.match(incsearch, hlstart, hlend) &&
			(HLSEARCH || file == thecurfile))
	{
		set_attrs(dir, file == thecurfile);

		addnstr(dir.name().c_str(), SYSmin(hlstart, maxlen));

		attrset(COLOR_PAIR(8));
		attron(A_REVERSE);
		addnstr(dir.name().c_str() + hlstart,
				SYSmax(SYSmin(hlend - hlstart, maxlen - hlstart), 0));

		set_attrs(dir, file == thecurfile);

		addnstr(dir.name().c_str() + hlend,
				SYSmax(SYSmin(dir.name().length() - hlend, maxlen - hlend), 0));
	}
	else
	{
		set_attrs(dir, file == thecurfile);
		addnstr(dir.name().c_str(), maxlen);
	}

	set_attrs(dir, false);

	move(2+y, xoff-1);
}

static void draw(const SPY_REGEX *incsearch = 0)
{
	char	username[BUFSIZE];
	char	hostname[BUFSIZE];
	char	title[BUFSIZE];

	gethostname(hostname, BUFSIZE);
	getlogin_r(username, BUFSIZE);

	// Use erase() to clear the screen before drawing. Don't use clear(),
	// since this will cause the next refresh() to clear the screen causing
	// flicker.
	erase();

	attrset(A_NORMAL);

	move(0, 0);
	snprintf(title, BUFSIZE, "%s@%s: %s", username, hostname, thecwd);
	addnstr(title, COLS);

	if (!themsg.empty())
	{
		move(LINES-1, 0);
		attrset(A_REVERSE);
		addnstr(themsg.c_str(), COLS-1);
		attrset(A_NORMAL);
	}

	if (thefiles.size())
	{
		if (thepages > 1)
		{
			move(1, 0);
			printw("Page %d/%d", thecurpage+1, thepages);
		}

		int file = thecurpage * thecols * therows;
		int maxfile = SYSmin((thecurpage+1) * thecols * therows, thefiles.size());
		for (; file < maxfile; file++)
		{
			if (file != thecurfile)
				drawfile(file, incsearch);
		}

		// Draw the current file last to leave the cursor in the expected
		// place
		drawfile(thecurfile, incsearch);
	}
	else
	{
		move(1, 0);
		printw("<empty>\n");
	}
}

static void redraw()
{
	rebuild();

	// Clear the screen for the next draw. This is for user-controlled redraw,
	// which should clear any garbage left on the screen by background jobs.
	clear();
}

static void ignoretoggle(const char *label)
{
	IGNOREMASK &mask = theignoremask[label];
	mask.myenable = !mask.myenable;

	rebuild();

	themsg = mask.myenable ? "Enabled" : "Disabled";
	themsg += " ignore mask '";
	themsg += label;
	themsg += "'";
}

static void detailtoggle()
{
	thedetail = (DETAIL_TYPE)((int)thedetail+1);
	if (thedetail == DETAIL_MAX)
		thedetail = DETAIL_NONE;

	rebuild();

	switch (thedetail)
	{
		case DETAIL_NONE: break;
		case DETAIL_SIZE: themsg = "Sorted by file size"; break;
		case DETAIL_TIME: themsg = "Sorted by modification time"; break;
	}
}

static void debugmode()
{
	thedebugmode = !thedebugmode;

	themsg = thedebugmode ? "Enabled" : "Disabled";
	themsg += " debug mode";
}

static int ncols()
{
	if (thecurpage < thepages-1)
		return thecols;

	int files = thefiles.size() - thecurpage*thecols*therows;
	return (files + therows - 1 - thecurrow) / therows;
}

static int nrows()
{
	if (thecurpage < thepages-1)
		return therows;

	int files = thefiles.size() - thecurpage*thecols*therows;
	files -= thecurcol * therows;
	if (files >= therows)
		return therows;
	if (files < 0)
		return 0;
	return files;
}

static void left()
{
	thecurcol--;
	if (thecurcol < 0)
		thecurcol = ncols()-1;
	pagetofile();
}

static void right()
{
	thecurcol++;
	if (thecurcol >= ncols())
		thecurcol = 0;
	pagetofile();
}

static void up()
{
	thecurrow--;
	if (thecurrow < 0)
		thecurrow = nrows()-1;
	pagetofile();
}

static void down()
{
	thecurrow++;
	if (thecurrow >= nrows())
		thecurrow = 0;
	pagetofile();
}

static bool spy_chdir(const char *dir)
{
	if (chdir(dir))
	{
		char	buf[BUFSIZE];
		themsg = strerror_r(errno, buf, BUFSIZE);
		return false;
	}

	// Check if the directory really changed before invoking rebuild(),
	// since special characters like '.' are handled directly by chdir.
	char cwd[FILENAME_MAX];
	if (!getcwd(cwd, sizeof(cwd)) || !strcmp(cwd, thecwd))
		return false;

	// Save the current file
	if (thecurfile < thefiles.size())
		thesavedcurfile[thecwd] = thefiles[thecurfile].name();

	// Save a copy of the directory name that we were just in (for
	// ".." handling below)
	std::string prevdir = thecwd;
	std::string prevparent;
	const size_t slashpos = prevdir.rfind('/');
	if (slashpos != std::string::npos)
	{
		prevparent = prevdir.substr(0, slashpos);
		prevdir = prevdir.substr(slashpos+1, prevdir.length()-slashpos-1);
	}

	rebuild();

	if (!strcmp(dir, "..") ||
		!strcmp(dir, prevparent.c_str()))
	{
		// Special case for ".." - in this case, I would like to see
		// the directory that we just came from as the current file.
		find_and_set_curfile(prevdir);
	}
	else
	{
		// Restore the previous file, if it existed
		auto it = thesavedcurfile.find(cwd);
		if (it != thesavedcurfile.end())
			find_and_set_curfile(it->second);
	}

	return true;
}

static bool spy_jump_dir(const char *dir)
{
	std::string expanded = dir;

	wordexp_t p;
	if (!wordexp(dir, &p, 0))
	{
		// Use the first valid expansion
		for (int i = 0; i < p.we_wordc; i++)
		{
			if (p.we_wordv[i])
			{
				expanded = p.we_wordv[i];
			}
		}
		wordfree(&p);
	}

	return spy_chdir(expanded.c_str());
}

static void jump_dir(const char *dir)
{
	spy_jump_dir(dir);

	draw();
	refresh();
}

static void dirup()
{
	if (!spy_jump_dir(".."))
	{
		themsg = "No parent directory";
	}
}

enum PROMPT_TYPE {
	// Stay in curses mode after executing the command. If there was
	// output, it won't be visible. This mode should only be used for
	// commands that don't require user input.
	PROMPT_SILENT,

	// Leave curses mode to show output, but return to curses mode
	// immediately after the command completes. This allows for interactive
	// commands to use the terminal but to return immediately to curses
	// mode when they complete.
	PROMPT_INTERACTIVE,

	// Leave curses mode and wait for input before returning to curses
	// mode. This mode shows the 'continue' prompt in termcap, without
	// returning to curses mode - so that the command output is visible
	// even after the command completes.
	PROMPT_CONTINUE
};

template <PROMPT_TYPE> static void execute_command(const char *);

static void dirdown_enter()
{
	if (!spy_jump_dir(thefiles[thecurfile].name().c_str()))
	{
		themsg.clear();
		std::string cmd = s_editor ? s_editor : "vim";
		cmd += " %";
		execute_command<PROMPT_SILENT>(cmd.c_str());
	}
}

static void dirdown_display()
{
	if (!spy_jump_dir(thefiles[thecurfile].name().c_str()))
	{
		themsg.clear();
		std::string cmd = s_pager ? s_pager : "less";
		cmd += " %";
		execute_command<PROMPT_SILENT>(cmd.c_str());
	}
}

static void pageup()
{
	if (thecurpage > 0)
	{
		thecurpage--;
		thecurfile -= therows * thecols;
	}
}
static void pagedown()
{
	if (thecurpage < thepages-1)
	{
		thecurpage++;
		thecurfile += therows * thecols;
		if (thecurfile >= thefiles.size())
		{
			thecurfile = thefiles.size()-1;
			filetopage();
		}
	}
}

static void firstfile()
{
	thecurfile = 0;
	filetopage();
}

static void lastfile()
{
	thecurfile = thefiles.size() ? thefiles.size()-1 : 0;
	filetopage();
}

static void take()
{
	themsg = "take not implemented";
}

static void setenv()
{
	themsg = "setenv not implemented";
}

static void ignore()
{
}

static int spy_getchar()
{
	int ch;

	if (isendwin())
	{
		// Set raw mode for stdin temporarily so that we can read a single
		// character of unbuffered input.
		struct termios old_settings;
		struct termios new_settings;

		tcgetattr(0, &old_settings);

		new_settings = old_settings;
		cfmakeraw (&new_settings);
		tcsetattr (0, TCSANOW, &new_settings);

		ch = getchar();

		tcsetattr (0, TCSANOW, &old_settings);

		if (ch == '\n' || ch == '\r')
			ch = '\n';

		// Hack to allow the arrow keys and delete to work in vi insert mode.
		// Unfortunately, this breaks ESC (you have to hit it twice)
		if (ch == ESC && rl_editing_mode == 0)
		{
			int ch2 = getchar();
			if (ch2 == '[')
			{
				int ch3 = getchar();
				switch (ch3)
				{
					case 'A':
						ch = KEY_UP; break;
					case 'B':
						ch = KEY_DOWN; break;
					case 'C':
						ch = KEY_RIGHT; break;
					case 'D':
						ch = KEY_LEFT; break;
					case '3':
						{
							int ch4 = getchar();
							if (ch4 == '~')
							{
								ch = KEY_DC;
								break;
							}
							ungetc(ch4, stdin);
						}
					default:
						ungetc(ch3, stdin);
						ungetc(ch2, stdin);
						break;
				}
			}
			else
			{
				ungetc(ch2, stdin);
			}
		}
	}
	else
	{
		// Curses input
		ch = getch();
	}

	return ch;
}

// Hackery to keep track of whether we're in vi command mode, since
// readline does not provide this state flag.
static bool thecommandmode = false;

static int spy_rl_getc(FILE *fp)
{
	int key = spy_getchar();

	switch (key)
	{
		case '\b':
		case KEY_BACKSPACE:
		case RUBOUT:
			if (!rl_point)
			{
				// Backspace past the prompt cancels the command in spy
				*rl_line_buffer = '\0';
				key = EOF;
			}
			else
			{
				key = '\b'; // Backspace character
			}
			break;

		case ERR:
			key = 0;
			break;

		case 'a':
		case 'A':
		case 'i':
		case 'I':
		case 'c':
		case 'C':
		case 's':
		case 'S':
			thecommandmode = false;
			break;

		case ESC:
		case KEY_DC:
			key = ESC;
			thecommandmode = true;
			break;
		//case KEY_DC:    key = 0; rl_delete(1, 0); break;

		// For the rl_display function to be called, we have to return
		// something - so return a null key (and assume readline ignores it
		// and refreshes the display).
		case KEY_UP:    key = 0; rl_get_previous_history(1, 0); break;
		case KEY_DOWN:  key = 0; rl_get_next_history(1, 0); break;
		case KEY_RIGHT: key = 0; rl_forward_char(1, 0); break;
		case KEY_LEFT:  key = 0; rl_backward_char(1, 0); break;
	}
	return key;
}

enum RLTYPE {
	JUMP,
	SEARCHNEXT,
	SEARCHPREV,
	EXECUTE
};

template <RLTYPE TYPE>
static inline int nextfile(int file) { return file < thefiles.size()-1 ? file+1 : 0; }

template <>
inline int nextfile<SEARCHPREV>(int file) { return file > 0 ? file-1 : thefiles.size()-1; }

template <RLTYPE TYPE>
static void searchnext()
{
	if (!thesearch)
		return;

	// Only search files other than thecurfile
	int file;
	for (file = nextfile<TYPE>(thecurfile); file != thecurfile; file = nextfile<TYPE>(file))
	{
		if (thefiles[file].match(thesearch.get()))
			break;
	}

	if (file != thecurfile)
	{
		thecurfile = file;
		filetopage();
	}
}

static char s_termcap_buf[2048];

static char *s_mr = 0;
static char *s_md = 0;
static char *s_me = 0;
static char *s_cr = 0;
static char *s_ce = 0;
static char *s_cm = 0;
static char *s_cd = 0;

static void init_termcap()
{
	char *ptr = s_termcap_buf;

	s_mr = tgetstr ("mr", &ptr); // Enter reverse mode
	s_md = tgetstr ("md", &ptr); // Enter bold mode
	s_me = tgetstr ("me", &ptr); // Exit all formatting modes
	s_cr = tgetstr ("cr", &ptr); // Move to start of line
	s_ce = tgetstr ("ce", &ptr); // Clear to end of line
	s_cm = tgetstr ("cm", &ptr); // Cursor movement
	s_cd = tgetstr ("cd", &ptr); // Clear to end of screen
}

static int thepromptline = 0;

template <RLTYPE TYPE>
static void spy_rl_display()
{
	if (!isendwin())
	{
		if ((TYPE == SEARCHNEXT || TYPE == SEARCHPREV)
			   && rl_line_buffer && *rl_line_buffer)
		{
			// Temporarily replace the current file to show what was found
			int prevfile = thecurfile;

			thesearch.reset(new SPY_REGEX(rl_line_buffer));
			searchnext<TYPE>();

			draw(thesearch.get());

			thesearch.reset(0);

			thecurfile = prevfile;
			filetopage();
		}
		else
		{
			draw();
		}

		attrset(A_NORMAL);

		// Print the prompt
		move(LINES-1, 0);
		addstr(rl_prompt);

		int off = getcurx(stdscr);

		addstr(rl_line_buffer);

		// Move to the cursor position
		move(LINES-1, off + rl_point);

		// Change the cursor color when we're in vi command mode
		if (rl_editing_mode == 0 && thecommandmode)
			chgat(1, A_NORMAL, 8, NULL);

		refresh();
	}
	else
	{
		tputs(tgoto(s_cm, 0, thepromptline), 1, putchar);
		tputs(s_cd, 1, putchar); // Necessary to clear lingering "Continue: "
		tputs(rl_prompt, 1, putchar);
		tputs(rl_line_buffer, 1, putchar);

		thepromptline = SYSmin(thepromptline, (LINES-1) -
			((strlen(rl_prompt) + strlen(rl_line_buffer) + 1) / COLS));

		int thecurscol = strlen(rl_prompt) + rl_point;
		int thecursline = thecurscol / COLS;

		thecurscol -= thecursline * COLS;
		thecursline += thepromptline;

		// Move to the cursor position
		tputs(tgoto(s_cm, thecurscol, thecursline), 1, putchar);
	}
}

static void add_unique_history(const char *command)
{
	for (int i = history_length; i-- > 0; )
	{
		// history_get() uses an offset of history_base
		const char *line = history_get(history_base+i)->line;
		if (!strcmp(line, command))
		{
			HIST_ENTRY *hist = remove_history(i);
			free(hist->line);
			free(hist);
			break;
		}
	}

	add_history(command);
}

static void continue_prompt(const char *status = 0)
{
	tputs(tgoto(s_cm, 0, LINES-1), 1, putchar);
	tputs(s_mr, 1, putchar);
	if (status && *status)
	{
		tputs(status, 1, putchar);
		tputs(". ", 1, putchar);
	}
	tputs("Continue: ", 1, putchar);
	tputs(s_me, 1, putchar);
	tputs(s_ce, 1, putchar);
}

static bool cancel_prompt()
{
	if (!isendwin())
	{
		draw();
		refresh();
	}
	else
	{
		continue_prompt();
	}
}

// Class to set and restore history state within a scope
class HISTORY_SCOPE {
public:
	HISTORY_SCOPE(HISTORY_STATE &state) : mystate(state)
	{
		history_set_history_state(&mystate);
	}
	~HISTORY_SCOPE()
	{
		HISTORY_STATE *tmp = history_get_history_state();
		mystate = *tmp;
		free(tmp);
	}

private:
	HISTORY_STATE &mystate;
};

static void jump()
{
	HISTORY_SCOPE scope(s_jump_history);

	// Configure readline
	rl_redisplay_function = spy_rl_display<JUMP>;

	// Try to find a good default jump target in the recent history, that
	// isn't the cwd.
	std::string lastjump = "~";
	for (int i = history_length; i-- > 0; )
	{
		// history_get() uses an offset of history_base
		const char *line = history_get(history_base+i)->line;
		if (strcmp(line, thecwd))
		{
			lastjump = line;
			break;
		}
	}

	// Read input
	std::string prompt = "Jump:  (";
	prompt += lastjump;
	prompt += ") ";
	char *input = readline(prompt.c_str());

	if (input)
	{
		std::string dir = *input ? input : lastjump;
		free(input);

		// Store the current directory
		add_unique_history(thecwd);

		spy_jump_dir(dir.c_str());

		draw();
		refresh();
	}
	else
	{
		cancel_prompt();
	}
}

template <RLTYPE TYPE>
static void search()
{
	HISTORY_SCOPE scope(s_search_history);

	if (thesearch)
	{
		thesearch.reset(0);
	}

	// Configure readline
	rl_redisplay_function = spy_rl_display<TYPE>;

	// Read input
	char *search = readline("/");

	if (search)
	{
		if (*search)
		{
			add_unique_history(search);

			thesearch.reset(new SPY_REGEX(search));
		}

		free(search);

		searchnext<TYPE>();

		draw();
		refresh();
	}
	else
	{
		cancel_prompt();
	}
}

static bool needs_quotes(const std::string &str)
{
	for (auto it = str.begin(); it != str.end(); ++it)
	{
		char c = *it;
		if (!isalpha(c) && !isdigit(c) && c != '_' && c != '.' && c != '-')
			return true;
	}

	return false;
}

// Expand special command characters
static std::string expand_command(const char *command)
{
	std::string expanded = command;

	if (thecurfile < thefiles.size())
	{
		std::string filename = thefiles[thecurfile].name();
		if (needs_quotes(filename))
		{
			filename = std::string("'") + filename + "'";
		}
		replaceall_non_escaped(expanded, '%', filename);
	}

	return expanded;
}

static const char *signalname(int signal)
{
	switch (signal)
	{
		case SIGHUP:	return "SIGHUP";
		case SIGINT:	return "SIGINT";
		case SIGQUIT:	return "SIGQUIT";
		case SIGILL:	return "SIGILL";
		case SIGTRAP:	return "SIGTRAP";
		case SIGABRT:	return "SIGABRT";
		case SIGBUS:	return "SIGBUS";
		case SIGFPE:	return "SIGFPE";
		case SIGKILL:	return "SIGKILL";
		case SIGUSR1:	return "SIGUSR1";
		case SIGSEGV:	return "SIGSEGV";
		case SIGUSR2:	return "SIGUSR2";
		case SIGPIPE:	return "SIGPIPE";
		case SIGALRM:	return "SIGALRM";
		case SIGTERM:	return "SIGTERM";
		case SIGSTKFLT:	return "SIGSTKFLT";
		case SIGCHLD:	return "SIGCHLD";
		case SIGCONT:	return "SIGCONT";
		case SIGSTOP:	return "SIGSTOP";
		case SIGTSTP:	return "SIGTSTP";
		case SIGTTIN:	return "SIGTTIN";
		case SIGTTOU:	return "SIGTTOU";
		case SIGURG:	return "SIGURG";
		case SIGXCPU:	return "SIGXCPU";
		case SIGXFSZ:	return "SIGXFSZ";
		case SIGVTALRM:	return "SIGVTALRM";
		case SIGPROF:	return "SIGPROF";
		case SIGWINCH:	return "SIGWINCH";
		case SIGIO:		return "SIGIO";
		case SIGPWR:	return "SIGPWR";
		case SIGSYS:	return "SIGSYS";
		case SIGSTKSZ:	return "SIGSTKSZ";
		default:		return strsignal(signal);
	}
}

template <PROMPT_TYPE prompt>
static void execute_command(const char *command)
{
	// Expand special characters
	std::string expanded = expand_command(command);

	if (prompt != PROMPT_SILENT)
	{
		endwin();

		// Leave the expanded command in the output stream
		tputs(s_md, 1, putchar);
		tputs(tgoto(s_cm, 0, thepromptline), 1, putchar);
		tputs("!", 1, putchar);
		tputs(expanded.c_str(), 1, putchar);
		tputs(s_me, 1, putchar);
		tputs(s_ce, 1, putchar);
		tputs("\n", 1, putchar);

		thepromptline = LINES-1;
	}
	else
	{
		// Without leaving curses mode, reset the terminal to shell mode
		// for the child
		reset_shell_mode();
	}

	// Create a pipe to pass the result of pwd back from the child when
	// it's done execution. The shell syntax only seems to work in bash,
	// so exclude other shells.
	const char	*bash = "/bin/bash";
	const char	*shell = s_shell ? s_shell : bash;
	const int	 shlen = strlen(shell);
	const bool	 recover_cwd = shlen >= 4 && !strncmp(shell+shlen-4, "bash", 4);
	int			 fd[2];

	if (recover_cwd)
	{
		if (pipe(fd) < 0)
		{
			perror("pipe failed");
		}
	}

	thechild = fork();
	if (thechild == -1)
	{
		perror("fork failed");
	}
	else if (thechild == 0)
	{
		// Child
		char		 buf[BUFSIZE];
		const char	*args[256];
		int			 va_args = 0;

		if (recover_cwd)
		{
			// Append a command to pass the pwd up to the parent
			close(fd[0]);
			snprintf(buf, BUFSIZE, " && pwd >& %d", fd[1]);
			expanded += buf;
		}

		// Execute commands in a subshell
		args[va_args++] = shell;
		args[va_args++] = "-c";
		args[va_args++] = expanded.c_str();
		args[va_args++] = 0;

		if (execvp(args[0], (char * const *)args) == -1)
		{
			perror("exec failed");
			exit(1);
		}

		// Unreachable
	}

	// Parent
	if (recover_cwd)
	{
		close(fd[1]);

		// Wait for the pwd
		char	buf[BUFSIZE];
		int		bytes = read(fd[0], buf, BUFSIZE);
		if (bytes > 1)
		{
			buf[bytes-1] = '\0';
			spy_chdir(buf);
		}

		close(fd[0]);
	}

	// Reap the child process
	int status;
	waitpid(thechild, &status, 0);

	thechild = 0;

	// If the exit status was non-zero, print some information about what
	// caused the process to exit.
	std::string status_string;
	if (WIFEXITED(status))
	{
		int exit_status = WEXITSTATUS(status);
		if (exit_status)
		{
			char buf[BUFSIZE];
			snprintf(buf, BUFSIZE, "Exit status %d", exit_status);
			status_string = buf;
		}
	}
	else if (WIFSIGNALED(status))
	{
		if (WCOREDUMP(status))
		{
			status_string = "Core dumped";
		}
		else
		{
			char buf[BUFSIZE];
			snprintf(buf, BUFSIZE, "Terminated by %s", signalname(WTERMSIG(status)));
			status_string = buf;
		}
	}

	if (prompt == PROMPT_CONTINUE)
	{
		continue_prompt(status_string.c_str());
	}
	else
	{
		if (prompt == PROMPT_SILENT)
			endwin();

		if (!status_string.empty())
			themsg = status_string;

		draw();
		refresh();
	}
}

static void execute()
{
	HISTORY_SCOPE scope(s_execute_history);

	// Configure readline
	rl_redisplay_function = spy_rl_display<EXECUTE>;

	// Read input
	char *command = readline("!");

	if (!command || !*command)
	{
		cancel_prompt();

		if (command)
			free(command);

		return;
	}

	add_unique_history(command);

	execute_command<PROMPT_CONTINUE>(command);

	free(command);
}

template <PROMPT_TYPE prompt>
static void prompt_command(const char *base)
{
	HISTORY_SCOPE scope(s_execute_history);

	// Configure readline
	rl_redisplay_function = spy_rl_display<EXECUTE>;

	// Read input
	std::string command = base;
	char *args = readline((command + ": ").c_str());

	if (!args)
	{
		cancel_prompt();
		return;
	}

	command += " ";
	command += args;

	add_unique_history(command.c_str());

	execute_command<prompt>(command.c_str());

	free(args);
}

static void last_command()
{
	HISTORY_SCOPE scope(s_execute_history);

	const HIST_ENTRY *hist = history_get(history_base+history_length-1);

	if (hist)
	{
		execute_command<PROMPT_CONTINUE>(hist->line);
	}
	else
	{
		themsg = "No previous command";
		
		draw();
		refresh();
	}
}

static void show_command()
{
	endwin();
	continue_prompt();
}

static void quit_prep()
{
	if (!isendwin())
	{
		endwin();
	}
	else
	{
		tputs(tgoto(s_cm, 0, LINES-1), 1, putchar);
		tputs(s_ce, 1, putchar); // Necessary to clear lingering "Continue: "
	}

	// Save jump history
	{
		HISTORY_SCOPE scope(s_jump_history);

		// Add the cwd to the history first, since often I'll want to jump
		// there upon restart
		if (*thecwd)
		{
			add_unique_history(thecwd);
		}
		if (write_history(s_jhistoryfile.c_str()))
		{
			fprintf(stderr, "warning: Could not write history file %s\n",
					s_jhistoryfile.c_str());
		}
	}

	// Save command history
	{
		HISTORY_SCOPE scope(s_execute_history);
		if (write_history(s_chistoryfile.c_str()))
		{
			fprintf(stderr, "warning: Could not write history file %s\n",
					s_chistoryfile.c_str());
		}
	}
}

static char **theargv = 0;

static void reload()
{
	quit_prep();

	if (execvp(theargv[0], (char * const *)theargv) == -1)
	{
		perror("exec failed");
		exit(1);
	}

	// Unreachable
}

typedef void(*VOIDFN)();
typedef void(*STRFN)(const char *);

class CALLBACK {
public:
	CALLBACK(const char *name = 0)
		: myname(name)
		, myvfn(0)
		, mysfn(0)
		, mydraw(true)
		{}
	CALLBACK(const char *name, VOIDFN fn)
		: myname(name)
		, myvfn(fn)
		, mysfn(0)
		, mydraw(true)
		{}
	CALLBACK(const char *name, STRFN fn)
		: myname(name)
		, myvfn(0)
		, mysfn(fn)
		, mydraw(true)
		{}
	CALLBACK(const char *name, VOIDFN vn, STRFN sn, bool draw)
		: myname(name)
		, myvfn(vn)
		, mysfn(sn)
		, mydraw(draw)
		{}

	void operator()() const
	{
		if (!mystr.empty())
			mysfn(mystr.c_str());
		else
			myvfn();

		if (mydraw)
		{
			draw();
			refresh();
		}
	}

	const char *name() const { return myname; }
	const char *str() const { return mystr.c_str(); }

	bool has_vfn() const { return myvfn; }
	bool has_sfn() const { return mysfn; }

	void set_str(const std::string &str)
	{
		// For some reason, .spyrc prefixes the jump_dir and ignoretoggle
		// with '='
		if (mysfn == jump_dir || mysfn == ignoretoggle)
			mystr = std::string(str.begin()+1, str.end());
		else
			mystr = str;
	}

private:
	const char	*myname;
	VOIDFN		 myvfn;
	STRFN		 mysfn;
	std::string	 mystr;
	bool		 mydraw;
};

static std::map<std::string, CALLBACK> thecommands;
static std::map<int, CALLBACK> thekeys;

static void help()
{
	// Without leaving curses mode, reset the terminal to shell mode
	// for the child
	reset_shell_mode();

	FILE *pipe = popen("less", "w");

	// Write
	fprintf(pipe, "Shell: %s\n", s_shell ? s_shell : "/bin/bash");
	fprintf(pipe, "\n");

	fprintf(pipe, "Key mappings:\n");
	std::map<std::string, CALLBACK> unmapped = thecommands;
	for (auto it = thekeys.begin(); it != thekeys.end(); ++it)
	{
		std::string key = "\'";
		key += keyname(it->first);
		key += "\'";

		fprintf(pipe, "%-15s %-13s %-s\n",
				key.c_str(),
				it->second.name(),
				it->second.str());

		unmapped.erase(it->second.name());
	}

	if (!unmapped.empty())
	{
		fprintf(pipe, "\nCommands without a key mapping:\n");
		for (auto it = unmapped.begin(); it != unmapped.end(); ++it)
		{
			fprintf(pipe, "%-15s %s\n", "", it->second.name());
		}
	}

	pclose(pipe);

	thechild = 0;

	endwin();
}

static void read_spyrc(std::istream &is,
		const std::map<std::string, CALLBACK> &commands,
		std::map<int, CALLBACK> &keys)
{
	// Build a reverse map for all keys
	std::map<std::string, int> keymap;
	for (int i = 0; i < KEY_MAX; i++)
	{
		const char *name = keyname(i);
		if (name)
			keymap[name] = i;
	}

	// There's probably another mapping we should use
	keymap["<Enter>"] = '\n';
	keymap["<Space>"] = ' ';

	std::map<std::string, int> colormap;
	colormap["black"] = COLOR_BLACK;
	colormap["red"] = COLOR_RED;
	colormap["green"] = COLOR_GREEN;
	colormap["yellow"] = COLOR_YELLOW;
	colormap["blue"] = COLOR_BLUE;
	colormap["magenta"] = COLOR_MAGENTA;
	colormap["purple"] = COLOR_MAGENTA;
	colormap["cyan"] = COLOR_CYAN;
	colormap["white"] = COLOR_WHITE;

	std::string line;
	while (std::getline(is, line))
	{
		std::istringstream iss(line);

		std::string cmd;
		if (!(iss >> cmd))
			continue;

		if (cmd[0] == '#')
			continue;

		if (cmd == "map")
		{
			std::string keystr;
			std::string command;

			if (!(iss >> keystr))
			{
				fprintf(stderr, "warning: Missing key\n");
				continue;
			}

			auto key_it = keymap.find(keystr);
			if (key_it == keymap.end())
			{
				fprintf(stderr, "warning: Unrecognized key %s\n", keystr.c_str());
				continue;
			}

			int key = key_it->second;

			if (!(iss >> command))
			{
				fprintf(stderr, "warning: Missing callback\n");
				continue;
			}

			auto command_it = commands.find(command);
			if (command_it == commands.end())
			{
				fprintf(stderr, "warning: Unrecognized callback %s\n", command.c_str());
				continue;
			}

			// The tail is read in 2 parts to skip whitespace, but
			// preserve whitespace characters in the command string
			std::string str, tail;
			iss >> str;
			std::getline(iss, tail);
			str += tail;

			CALLBACK cb = command_it->second;

			if (!str.empty())
			{
				if (!cb.has_sfn())
					fprintf(stderr, "warning: %s doesn't accept a string argument\n", command_it->first.c_str());
				else
					cb.set_str(str);
			}
			else
			{
				if (!cb.has_vfn())
					fprintf(stderr, "warning: %s requires a string argument\n", command_it->first.c_str());
			}

			keys[key] = cb;
		}
		else if (cmd == "relaxprompt" ||
				cmd == "relaxsearch" ||
				cmd == "relaxcase")
		{
			// Ignored
		}
		else if (cmd == "ignoremask")
		{
			std::string pattern;
			std::string index;
			if (!(iss >> pattern))
			{
				fprintf(stderr, "warning: Missing pattern\n");
				continue;
			}

			if (!(iss >> index))
				index = "0";

			theignoremask[index].mypatterns.push_back(pattern);
		}
		else if (cmd == "ignoredefault")
		{
			std::string index;
			int enable = 0;
			if (!(iss >> index))
			{
				fprintf(stderr, "warning: Missing index\n");
				continue;
			}
			if (!(iss >> enable))
			{
				fprintf(stderr, "warning: Missing enable\n");
				continue;
			}

			theignoremask[index].myenable = enable;
		}
		else if (cmd == "color")
		{
			std::string pattern;
			std::string color;

			if (!(iss >> pattern))
			{
				fprintf(stderr, "warning: Missing pattern\n");
				continue;
			}
			if (!(iss >> color))
			{
				fprintf(stderr, "warning: Missing color\n");
				continue;
			}

			auto color_it = colormap.find(color);
			if (color_it == colormap.end())
			{
				fprintf(stderr, "warning: Unknown color: %s\n", color.c_str());
				continue;
			}

			thecolors.push_back(COLOR(pattern, color_it->second));
		}
		else
		{
			fprintf(stderr, "warning: Unrecognized command %s\n", cmd.c_str());
		}
	}
}

static void spy_rl_display_match_list (char **matches, int len, int max)
{
	// TODO
}

static void spy_rl_prep_terminal(int)
{
	thecommandmode = false;
	thepromptline = LINES-1;
}

static void spy_rl_deprep_terminal()
{
}

static void init_curses()
{
	// Using newterm() instead of initscr() is supposed to avoid stdout
	// buffering problems with child processes
	newterm(getenv("TERM"), fopen("/dev/tty", "w"), fopen("/dev/tty", "r"));
	//initscr();

	// This is required for the arrow and backspace keys to function
	// correctly
	keypad(stdscr, true);

	cbreak(); // Accept characters immediately without waiting for NL
	noecho(); // Don't echo input to the screen

	// This is required to wrap long command lines. It does not actually
	// allow scrolling with the mouse wheel
	scrollok(stdscr, true);

	// Block for 1s in getch(). ERR is returned on timout, so we can handle
	// resize events.
	// NOTE: Even with a 1s timeout, curses seems to give us ERR keys
	// faster than that while the window is being resized.
	timeout(1000);

	ESCDELAY = 0;

	if (has_colors())
	{
		start_color();

		use_default_colors();

		// Store foreground pair to match COLOR_* by index
		for (int i = 1; i <= COLOR_WHITE; i++)
			init_pair(i, i, -1);

		// Inverted magenta for search coloring
		init_pair(8, COLOR_MAGENTA, -1);
	}
}

static void load_history(const std::string &fname, HISTORY_STATE &state)
{
	HISTORY_SCOPE scope(state);
	read_history(fname.c_str());
}

static void init_readline()
{
	load_history(s_jhistoryfile, s_jump_history);
	load_history(s_chistoryfile, s_execute_history);

	rl_getc_function = spy_rl_getc;
	rl_prep_term_function = spy_rl_prep_terminal;
	rl_deprep_term_function = spy_rl_deprep_terminal;
	rl_outstream = 0;
	rl_completion_display_matches_hook = spy_rl_display_match_list;
	rl_readline_name = "spy";

	// rl_initialize() sets the LC_CTYPE locale - set it back to the
	// default "C" after the call:
	// http://stackoverflow.com/questions/25457569/why-does-the-first-call-to-readline-slow-down-all-subsequent-calls-to-fnmatch
	rl_initialize();
	setlocale(LC_CTYPE, "C");
}

static CALLBACK thecallbacks[] = {
	CALLBACK("down", down),
	CALLBACK("up", up),
	CALLBACK("left", left),
	CALLBACK("right", right),

	CALLBACK("display", dirdown_display),
	CALLBACK("enter", dirdown_enter),
	CALLBACK("climb", dirup),

	CALLBACK("pagedown", pagedown),
	CALLBACK("pageup", pageup),
	CALLBACK("firstfile", firstfile),
	CALLBACK("lastfile", lastfile),

	CALLBACK("quit", quit),

	CALLBACK("jump", jump, jump_dir, false),

	CALLBACK("search", search<SEARCHNEXT>, 0, false),
	CALLBACK("next", searchnext<SEARCHNEXT>),
	CALLBACK("prev", searchnext<SEARCHPREV>),

	CALLBACK("unix_cmd", execute, 0, false),

	CALLBACK("unix", 0, execute_command<PROMPT_CONTINUE>, false),
	CALLBACK("unix_silent", 0, execute_command<PROMPT_SILENT>, false),
	CALLBACK("unix_interactive", 0, execute_command<PROMPT_INTERACTIVE>, false),
	CALLBACK("prompt", 0, prompt_command<PROMPT_CONTINUE>, false),
	CALLBACK("prompt_silent", 0, prompt_command<PROMPT_SILENT>, false),
	CALLBACK("prompt_interactive", 0, prompt_command<PROMPT_INTERACTIVE>, false),

	CALLBACK("last_cmd", last_command, 0, false),
	CALLBACK("show_cmd", show_command, 0, false),

	CALLBACK("redraw", redraw),

	CALLBACK("loadrc", reload),

	CALLBACK("ignoretoggle", ignoretoggle),
	CALLBACK("detailtoggle", detailtoggle),

	CALLBACK("debugmode", debugmode),

	CALLBACK("take", take),
	CALLBACK("setenv", setenv),
	CALLBACK("ignore", ignore),

	CALLBACK("help", help),
};

int main(int argc, char *argv[])
{
	// Retain the initial arguments for reload()
	theargv = argv;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGWINCH, signal_resize);

	for (int i = 0; i < sizeof(thecallbacks)/sizeof(CALLBACK); i++)
	{
		thecommands[thecallbacks[i].name()] = thecallbacks[i];
	}

	// Install default keybindings
	{
		std::stringstream is((const char *)spyrc_defaults);
		if (is)
			read_spyrc(is, thecommands, thekeys);
	}

	// Install user keybindings
	{
		// Try loading from .spyrc then $HOME/.spyrc
		std::string dir = ".spyrc";
		std::ifstream is(dir);
		if (!is)
		{
			dir = s_home;
			dir += "/.spyrc";
			is.open(dir);
		}
		if (is)
			read_spyrc(is, thecommands, thekeys);
	}

	init_readline();
	init_termcap();
	init_curses();

	rebuild();
	draw();
	refresh();

	thepromptline = LINES-1;

	while (true)
	{
		int c = spy_getchar();

		if (!isendwin() && theresized)
		{
			theresized = false;

			struct winsize w;
			ioctl(0, TIOCGWINSZ, &w);
			resize_term(w.ws_row, w.ws_col);

			redraw();
			draw();
			refresh();
		}

		if (c == ERR)
			continue;

		if (isendwin())
		{
			// Clear the continue prompt
			tputs(s_cr, 1, putchar);
			tputs(s_ce, 1, putchar);
		}

		auto it = thekeys.find(c);
		if (it != thekeys.end())
		{
			themsg.clear();
			it->second();
		}
		else
		{
			char buf[BUFSIZE];
			snprintf(buf, BUFSIZE, "Key '%s' [%d] undefined", keyname(c), c);
			themsg = buf;

			if (!isendwin())
			{
				draw();
				refresh();
			}
		}
	}

	quit();
}


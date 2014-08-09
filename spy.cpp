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
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>

#include "spyrc_defaults.h"

// Compile time parameters (could be made settings)
static const int XPADDING = 1;
const bool RELAXCASE = true;
const bool HLSEARCH = false;

// Environment
const char *s_shell = getenv("SHELL");
const char *s_home = getenv("HOME");

// History
static HISTORY_STATE s_jump_history;
static HISTORY_STATE s_search_history;
static HISTORY_STATE s_execute_history;

// Child process
static int thechild = 0;

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
	}
}

static void quit()
{
	endwin();
	exit(0);
}

static void signal_handler(int sig)
{
	if (thechild)
	{
		kill(thechild, sig);
	}
	else
	{
		if (sig == SIGINT)
			quit();
	}
}

struct ci_equal {
	bool operator()(char ch1, char ch2) {
		return std::tolower(ch1) == std::tolower(ch2);
	}
};

// find substring (case insensitive)
int ci_find_substr(const std::string& str1, const std::string& str2)
{
	std::string::const_iterator it = std::search(
			str1.begin(), str1.end(), 
			str2.begin(), str2.end(),
			ci_equal());
	if (it != str1.end())
		return it - str1.begin();
	return -1; // not found
}

class DIRINFO {
public:
	DIRINFO() : mydirectory(false) {}

	const std::string &name() const { return myname; }
	void setname(const char *name) { myname = name; }

	bool isdirectory() const { return mydirectory; }
	void setdirectory() { mydirectory = true; }

	bool isexecute() const { lazy_stat(); return mystat->st_mode & S_IXUSR; }
	bool iswrite() const { lazy_stat(); return mystat->st_mode & S_IWUSR; }

	bool operator<(const DIRINFO &rhs) const
	{
		bool adir = isdirectory();
		bool bdir = rhs.isdirectory();
		if (adir != bdir)
			return adir > bdir;

		return strcasecmp(myname.c_str(), rhs.myname.c_str()) < 0;
	}

	bool match(const char *search) const { int off; return match(search, off); }
	bool match(const char *search, int &off) const
	{
		if (!search || !*search)
			return false;

		if (RELAXCASE)
		{
			off = ci_find_substr(myname, search);
		}
		else
		{
			off = myname.find(search);
			if (off == std::string::npos)
				off = -1;
		}
		return off >= 0;
	}

private:
	void lazy_stat() const
	{
		if (!mystat)
		{
			mystat.reset(new struct stat);
			stat(myname.c_str(), mystat.get());
		}
	}

	std::string myname;
	mutable std::shared_ptr<struct stat> mystat;
	bool mydirectory;
};

static const int BUFSIZE = 256;

// File/directory state
static std::vector<DIRINFO> thefiles;
static char thecwd[FILENAME_MAX];

// Current file/page
static int thecurfile = 0;
static int thecurpage = 0;
static int thecurcol = 0;
static int thecurrow = 0;

// Layout info
static int thepages = 0;
static int therows = 0;
static int thecols = 0;

// Search info
static char *thesearch = 0;

// Color info
struct COLOR {
	enum COLORTYPE {
		DIRECTORY,
		EXECUTABLE,
		READONLY,
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

static void layout(const std::vector<DIRINFO> &dirs, int ysize, int xsize)
{
	int maxwidth = 0;
	for (int i = 0; i < dirs.size(); i++)
	{
		maxwidth = SYSmax(maxwidth, dirs[i].name().length() + 2);
	}

	therows = ysize;
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

static void rebuild()
{
	// Get the directory listing
	if (!getcwd(thecwd, sizeof(thecwd)))
		exit(1);

	DIR *dp = opendir(thecwd);
	if (dp == NULL)
		exit(1);

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
				thefiles.back().setdirectory();
		}
		readdir_r(dp, &entry, &result);
	}

	std::sort(thefiles.begin(), thefiles.end());

	layout(thefiles, LINES-3, COLS);

	if (thecurfile >= thefiles.size())
	{
		thecurfile = thefiles.size() ? thefiles.size()-1 : 0;
		filetopage();
	}

	closedir(dp);
}

static void ignoretoggle(const char *label)
{
	IGNOREMASK &mask = theignoremask[label+1];
	mask.myenable = !mask.myenable;

	rebuild();
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

static void jump_dir(const char *dir)
{
	wordexp_t p;
	wordexp(dir, &p, 0);

	// Use the first valid expansion
	std::string expanded;
	for (int i = 0; i < p.we_wordc; i++)
	{
		if (p.we_wordv[i])
		{
			expanded = p.we_wordv[i];
		}
	}

	wordfree(&p);

	if (chdir(expanded.c_str()))
	{
		perror("chdir");
	}
	else
	{
		rebuild();
	}
}

static void dirup()
{
	jump_dir("..");
}

static void execute_command(const char *);

static void dirdown_enter()
{
	if (thefiles[thecurfile].isdirectory())
		jump_dir(thefiles[thecurfile].name().c_str());
	else
		execute_command("$EDITOR %");
}

static void dirdown_display()
{
	if (thefiles[thecurfile].isdirectory())
		jump_dir(thefiles[thecurfile].name().c_str());
	else
		execute_command("$PAGER %");
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

static void lastfile()
{
	thecurfile = thefiles.size() ? thefiles.size()-1 : 0;
	filetopage();
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

static void drawfile(int file, const char *incsearch)
{
	int page, x, y;
	filetopage(file, page, x, y);

	int xoff = (x * COLS) / thecols;

	const DIRINFO &dir = thefiles[file];

	set_attrs(dir, false);
	if (dir.isdirectory())
	{
		move(2+y, xoff);
		addch('*');
	}

	move(2+y, xoff+2);

	int off;
	if (dir.match(incsearch, off) && (HLSEARCH || file == thecurfile))
	{
		int hlstart = off;
		int hlend = off + strlen(incsearch);

		set_attrs(dir, file == thecurfile);

		addnstr(dir.name().c_str(), hlstart);

		attrset(COLOR_PAIR(8));
		attron(A_REVERSE);
		addnstr(dir.name().c_str() + hlstart, hlend - hlstart);

		set_attrs(dir, file == thecurfile);

		addnstr(dir.name().c_str() + hlend, dir.name().length()-hlend);
	}
	else
	{
		set_attrs(dir, file == thecurfile);
		addstr(dir.name().c_str());
	}

	set_attrs(dir, false);

	move(2+y, xoff+1);
}

static void draw(const char *incsearch = 0)
{
	char	username[BUFSIZE];
	char	hostname[BUFSIZE];

	gethostname(hostname, BUFSIZE);
	getlogin_r(username, BUFSIZE);

	clear();

	attrset(A_NORMAL);

	move(0, 0);
	printw("%s@%s %s", username, hostname, thecwd);

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
			drawfile(file, incsearch);
		}

		// Draw the current file a second time to leave the cursor in the
		// expected place
		drawfile(thecurfile, incsearch);
	}
	else
	{
		move(1, 0);
		printw("<empty>\n");
	}
}

static void invalid()
{
}

int spy_rl_getc(FILE *fp)
{
	int key = getch();
	switch (key)
	{
		case ESC:
			*rl_line_buffer = '\0';
			key = EOF;
			break;
		case KEY_BACKSPACE:
			if (!rl_point)
			{
				*rl_line_buffer = '\0';
				key = EOF;
			}
			else
			{
				key = RUBOUT;
			}
			break;
		case KEY_UP: key = CTRL('p'); break;
		case KEY_DOWN: key = CTRL('n'); break;
		case KEY_LEFT: key = CTRL('b'); break;
		case KEY_RIGHT: key = CTRL('f'); break;
	}
	return key;
}

static std::string lastjump = "~";

static int nextfile(int file) { return file < thefiles.size()-1 ? file+1 : 0; }

static void searchnext()
{
	if (!thesearch)
		return;

	// Only search files other than thecurfile
	int file;
	for (file = nextfile(thecurfile); file != thecurfile; file = nextfile(file))
	{
		if (thefiles[file].match(thesearch))
			break;
	}

	if (file != thecurfile)
	{
		thecurfile = file;
		filetopage();
	}
}

enum RLTYPE {
	JUMP,
	SEARCH,
	EXECUTE
};

template <RLTYPE TYPE>
void spy_rl_display()
{
	if (TYPE == SEARCH)
	{
		// Temporarily replace the current file to show what was found
		int prevfile = thecurfile;

		thesearch = rl_line_buffer;
		searchnext();

		draw(rl_line_buffer);

		thesearch = 0;
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
	if (TYPE == JUMP)
	{
		addstr("Jump:  (");
		addstr(lastjump.c_str());
		addstr(") ");
	}
	else if (TYPE == SEARCH)
	{
		addstr("/");
	}
	else if (TYPE == EXECUTE)
	{
		addstr("!");
	}

	int off = getcurx(stdscr);

	addstr(rl_line_buffer);

	// Move to the cursor position
	move(LINES-1, off + rl_point);

	refresh();
}

static void jump()
{
	// Configure readline
	rl_getc_function = spy_rl_getc;
	rl_redisplay_function = spy_rl_display<JUMP>;

	history_set_history_state(&s_jump_history);

	// Read input
	char *input = readline("");

	if (!input)
		return;

	std::string dir = *input ? input : lastjump.c_str();

	add_history(dir.c_str());
	s_jump_history = *history_get_history_state();

	// Store the current directory
	lastjump = thecwd;

	jump_dir(dir.c_str());

	free(input);
}

static void search()
{
	if (thesearch)
	{
		free(thesearch);
		thesearch = 0;
	}

	// Configure readline
	rl_getc_function = spy_rl_getc;
	rl_redisplay_function = spy_rl_display<SEARCH>;

	history_set_history_state(&s_search_history);

	// Read input
	thesearch = readline("");

	if (thesearch && *thesearch)
	{
		add_history(thesearch);
		s_search_history = *history_get_history_state();
	}

	searchnext();
}

static int getchar_unbuffered()
{
	// Set raw mode for stdin temporarily so that we can read a single
	// character of unbuffered input.
	struct termios old_settings;
	struct termios new_settings;

	tcgetattr(0, &old_settings);

	new_settings = old_settings;
	cfmakeraw (&new_settings);
	tcsetattr (0, TCSANOW, &new_settings);

	int ch = getchar();

	tcsetattr (0, TCSANOW, &old_settings);

	return ch;
}

static int continue_prompt()
{
	char buffer[2048];
	char *ptr = buffer;
	char *mr_string = tgetstr ("mr", &ptr); // Enter reverse mode
	char *me_string = tgetstr ("me", &ptr); // Exit all formatting modes

	tputs(mr_string, 1, putchar);
	tputs("Continue: ", 1, putchar);
	tputs(me_string, 1, putchar);

	int ch = getchar_unbuffered();
	tputs("\n", 1, putchar);

	return ch;
}

static int thependingch = 0;

static void execute_command(const char *command)
{
	// Temporarily end curses mode for command output
	endwin();

	// Leave the command in the output stream
	printf("!");
	printf("%s\n", command);

	thechild = fork();
	if (thechild == -1)
	{
		perror("fork failed");
	}
	else if (thechild == 0)
	{
		const char		*delim = " \t";
		const char      *args[256];
		int				 va_args = 0;

		// Expand special characters
		std::string expanded = command;

		if (thecurfile < thefiles.size())
			replaceall_non_escaped(expanded, '%', thefiles[thecurfile].name());

		// Execute commands in a subshell
		args[va_args++] = s_shell ? s_shell : "/bin/bash";
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

	int status;
	waitpid(thechild, &status, 0);

	thechild = 0;

	thependingch = continue_prompt();
}

static void execute()
{
	// Configure readline
	rl_getc_function = spy_rl_getc;
	rl_redisplay_function = spy_rl_display<EXECUTE>;

	history_set_history_state(&s_execute_history);

	// Read input
	char *command = readline("");

	if (!command)
		return;

	if (!*command)
	{
		free(command);
		return;
	}

	add_history(command);
	s_execute_history = *history_get_history_state();

	execute_command(command);

	free(command);
}

static char theinitialcwd[FILENAME_MAX];
static char **theargv = 0;

static void reload()
{
	endwin();

	if (chdir(theinitialcwd))
		exit(1);
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
	CALLBACK()
		: myvfn(0)
		, mysfn(0)
		{}
	CALLBACK(VOIDFN fn)
		: myvfn(fn)
		, mysfn(0)
		{}
	CALLBACK(STRFN fn)
		: myvfn(0)
		, mysfn(fn)
		{}
	CALLBACK(VOIDFN vn, STRFN sn)
		: myvfn(vn)
		, mysfn(sn)
		{}

	void operator()() const
	{
		if (!mystr.empty())
			mysfn(mystr.c_str());
		else
			myvfn();
	}

	bool has_vfn() const { return myvfn; }
	bool has_sfn() const { return mysfn; }

	void set_str(const std::string &str)
	{
		// For some reason, .spyrc prefixes the jump directory with '='
		if (mysfn == jump_dir)
			mystr = std::string(str.begin()+1, str.end());
		else
			mystr = str;
	}

private:
	VOIDFN		myvfn;
	STRFN		mysfn;
	std::string	mystr;
};

static void read_spyrc(std::istream &is,
		const std::map<std::string, CALLBACK> &commands,
		std::map<int, CALLBACK> &callbacks)
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

			callbacks[key] = cb;
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

static void start_curses()
{
	// Initialize curses

	// Using newterm() instead of initscr() is supposed to avoid stdout
	// buffering problems with child processes
	newterm(getenv("TERM"),
			fopen("/dev/tty", "w"),
			fopen("/dev/tty", "r"));
	//initscr();

	// This is required for the arrow and backspace keys to function
	// correctly
	keypad(stdscr, TRUE);

	//nonl();         /* tell curses not to do NL->CR/NL on output */
	//cbreak();       /* take input chars one at a time, no wait for \n */
	//echo();         /* echo input - in color */

	// This is required to wrap long command lines. It does not actually
	// allow scrolling with the mouse wheel
	scrollok(stdscr, true);

	ESCDELAY = 0;

	// Initialize readline
	s_jump_history = *history_get_history_state();
	s_search_history = *history_get_history_state();
	s_execute_history = *history_get_history_state();

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

int main(int argc, char *argv[])
{
	// Retain the initial directory for reload()
	if (!getcwd(theinitialcwd, sizeof(theinitialcwd)))
		exit(1);
	theargv = argv;

	signal(SIGINT, signal_handler);

	std::map<std::string, CALLBACK> commands;

	commands["down"] = down;
	commands["up"] = up;
	commands["left"] = left;
	commands["right"] = right;

	commands["display"] = dirdown_display;
	commands["enter"] = dirdown_enter;
	commands["climb"] = dirup;

	commands["pagedown"] = pagedown;
	commands["pageup"] = pageup;
	commands["lastfile"] = lastfile;

	commands["quit"] = quit;
	commands["invalid"] = invalid;

	commands["jump"] = CALLBACK(jump, jump_dir);

	commands["search"] = search;
	commands["next"] = searchnext;

	commands["unix_cmd"] = execute;
	commands["unix"] = execute_command;

	commands["redraw"] = rebuild;

	commands["loadrc"] = reload;

	commands["ignoretoggle"] = ignoretoggle;

	commands["take"] = invalid;
	commands["setenv"] = invalid;

	std::map<int, CALLBACK> callbacks;

	// Install default keybindings
	{
		std::stringstream is((const char *)spyrc_defaults);
		if (is)
			read_spyrc(is, commands, callbacks);
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
			read_spyrc(is, commands, callbacks);
	}

	start_curses();
	rebuild();
	while (true)
	{
		draw();
		refresh();

		int c;
		if (!thependingch)
		{
			c = getch();
		}
		else
		{
			c = thependingch;
			thependingch = 0;
		}

		auto it = callbacks.find(c);
		if (it != callbacks.end())
			it->second();
	}

	quit();
}


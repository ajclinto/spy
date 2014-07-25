#include <stdlib.h>
#include <curses.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <pwd.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/wait.h>

#include <string>
#include <vector>
#include <map>
#include <algorithm>

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

static void finish(int sig)
{
	endwin();

	exit(0);
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

struct DIRINFO {
	DIRINFO() {}
	DIRINFO(const char *name, unsigned char type)
		: myname(name)
		, mytype(type)
		{}

	bool operator<(const DIRINFO &rhs) const
	{
		bool adir = mytype == DT_DIR;
		bool bdir = rhs.mytype == DT_DIR;
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

	std::string myname;
	char mytype;
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

static void layout(const std::vector<DIRINFO> &dirs, int ysize, int xsize)
{
	int maxwidth = 0;
	for (int i = 0; i < dirs.size(); i++)
	{
		maxwidth = SYSmax(maxwidth, dirs[i].myname.length() + 2);
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

	struct dirent *entry;
	int y = 0;
	while((entry = readdir(dp)))
	{
		if (strcmp(entry->d_name, ".") &&
			strcmp(entry->d_name, ".."))
		{
			thefiles.push_back(DIRINFO());
			thefiles.back().myname = entry->d_name;
			thefiles.back().mytype = entry->d_type;
		}
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
	std::string expanded = dir;
	replaceall(expanded, "~", s_home);

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
static void dirdown()
{
	jump_dir(thefiles[thecurfile].myname.c_str());
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

static void set_attrs(char type, bool curfile)
{
	if (curfile)
	{
		attrset(COLOR_PAIR(0));
		attron(A_REVERSE);
	}
	else
	{
		switch (type)
		{
			case DT_DIR: attrset(COLOR_PAIR(3)); break;
			case DT_FIFO: attrset(COLOR_PAIR(1)); break;
			case DT_CHR: attrset(COLOR_PAIR(2)); break;
			case DT_BLK: attrset(COLOR_PAIR(4)); break;
			case DT_LNK: attrset(COLOR_PAIR(6)); break;
			case DT_SOCK: attrset(COLOR_PAIR(5)); break;
			case DT_WHT: attrset(COLOR_PAIR(5)); break;
			default: attrset(COLOR_PAIR(0)); break;
		}
	}
}

static void drawfile(int file, const char *incsearch)
{
	int page, x, y;
	filetopage(file, page, x, y);

	int xoff = (x * COLS) / thecols;

	const DIRINFO &dir = thefiles[file];

	set_attrs(dir.mytype, false);
	switch (dir.mytype)
	{
		case DT_DIR:
			move(2+y, xoff);
			addch('*');
			break;
	}

	move(2+y, xoff+2);

	int off;
	if (dir.match(incsearch, off) && (HLSEARCH || file == thecurfile))
	{
		int hlstart = off;
		int hlend = off + strlen(incsearch);

		set_attrs(dir.mytype, file == thecurfile);

		addnstr(dir.myname.c_str(), hlstart);

		attrset(COLOR_PAIR(6));
		attron(A_REVERSE);
		addnstr(dir.myname.c_str() + hlstart, hlend - hlstart);

		set_attrs(dir.mytype, file == thecurfile);

		addnstr(dir.myname.c_str() + hlend, dir.myname.length()-hlend);
	}
	else
	{
		set_attrs(dir.mytype, file == thecurfile);
		addstr(dir.myname.c_str());
	}

	switch (dir.mytype)
	{
		case DT_DIR:
			attrset(COLOR_PAIR(3));
			break;
		default:
			attrset(COLOR_PAIR(0));
			break;
	}

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

static void quit()
{
	finish(0);
}

int rl_getc(FILE *fp)
{
	int key = getch();
	switch (key)
	{
		case KEY_BACKSPACE:
			key = (*rl_line_buffer) ? RUBOUT : EOF;
			break;
		case KEY_UP:
			key = CTRL('p');
			break;
		case KEY_DOWN:
			key = CTRL('n');
			break;
	}
	return key;
}

static const char *lastjump = "~";

void jump_rl_display()
{
	draw();

	attrset(A_NORMAL);

	// Print the prompt
	move(LINES-1, 0);
	addstr("Jump:  (");
	addstr(lastjump);
	addstr(") ");

	addstr(rl_line_buffer);

	refresh();
}

static void jump()
{
	// Configure readline
	rl_getc_function = rl_getc;
	rl_redisplay_function = jump_rl_display;

	history_set_history_state(&s_jump_history);

	// Read input
	char *input = readline("");

	if (!input)
		return;

	const char *dir = *input ? input : lastjump;

	add_history(dir);
	s_jump_history = *history_get_history_state();

	jump_dir(dir);

	free(input);
}

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

void search_rl_display()
{
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

	attrset(A_NORMAL);

	// Print the prompt
	move(LINES-1, 0);
	addstr("/");

	addstr(rl_line_buffer);

	refresh();
}

static void search()
{
	if (thesearch)
	{
		free(thesearch);
		thesearch = 0;
	}

	// Configure readline
	rl_getc_function = rl_getc;
	rl_redisplay_function = search_rl_display;

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

void execute_rl_display()
{
	draw();

	attrset(A_NORMAL);

	// Print the prompt
	move(LINES-1, 0);
	addstr("!");

	addstr(rl_line_buffer);

	refresh();
}

static void execute_command(const char *command)
{
	// Temporarily end curses mode for command output
	endwin();

	int child = fork();
	if (child == -1)
	{
		perror("fork failed");
	}
	else if (child == 0)
	{
		const char		*delim = " \t";
		const char      *args[256];
		int				 va_args = 0;

		// Expand special characters
		std::string expanded = command;

		// TODO: There should be support for escaping %
		if (thecurfile < thefiles.size())
			replaceall(expanded, "%", thefiles[thecurfile].myname);
		replaceall(expanded, "~", s_home);

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
	waitpid(child, &status, 0);

	// Wait for any key
	// If the command produced no output, we shouldn't bother with this
	getch();
}

static void execute()
{
	// Configure readline
	rl_getc_function = rl_getc;
	rl_redisplay_function = execute_rl_display;

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
	CALLBACK(STRFN fn, const char *str)
		: myvfn(0)
		, mysfn(fn)
		, mystr(str)
		{}

	void operator()() const
	{
		if (mysfn)
			mysfn(mystr.c_str());
		else
			myvfn();
	}

private:
	VOIDFN		myvfn;
	STRFN		mysfn;
	std::string	mystr;
};

int main(int argc, char *argv[])
{
	// TODO: Broken
	signal(SIGINT, finish);

	// Initialize curses
	initscr();

	// This is required for the arrow and backspace keys to function
	// correctly
	keypad(stdscr, TRUE);

	//nonl();         /* tell curses not to do NL->CR/NL on output */
	//cbreak();       /* take input chars one at a time, no wait for \n */
	//echo();         /* echo input - in color */

	// Initialize readline
	s_jump_history = *history_get_history_state();
	s_search_history = *history_get_history_state();
	s_execute_history = *history_get_history_state();

	if (has_colors())
	{
		start_color();

		use_default_colors();

		init_pair(1, COLOR_RED, -1);
		init_pair(2, COLOR_GREEN, -1);
		init_pair(3, COLOR_YELLOW, -1);
		init_pair(4, COLOR_BLUE, -1);
		init_pair(5, COLOR_CYAN, -1);
		init_pair(6, COLOR_MAGENTA, -1);
		init_pair(7, -1, COLOR_MAGENTA);
	}

	std::map<int, CALLBACK> commands;

	commands['j'] = commands[KEY_DOWN] = down;
	commands['k'] = commands[KEY_UP] = up;
	commands['h'] = commands[KEY_LEFT] = left;
	commands['l'] = commands[KEY_RIGHT] = right;

	commands['d'] = dirdown;
	commands['u'] = dirup;

	commands['r'] = commands[KEY_NPAGE] = pagedown;
	commands['t'] = commands[KEY_PPAGE] = pageup;
	commands['G'] = lastfile;

	commands['q'] = quit;

	commands['g'] = jump;

	commands['/'] = search;
	commands['n'] = searchnext;

	commands['!'] = commands[':'] = commands[';'] = execute;

	commands['v'] = CALLBACK(execute_command, "$EDITOR %");
	commands['L'] = CALLBACK(execute_command, "ls -l %");

	// These should probably be sourced from .spyrc
	commands['f'] = CALLBACK(execute_command, "file %");
	commands['m'] = CALLBACK(execute_command, "make -j4");

	commands['1'] = CALLBACK(jump_dir, "~/projects/spy");

	rebuild();
	while (true)
	{
		draw();
		refresh();

		int c = getch();
		if (commands.count(c))
			commands[c]();
	}

	finish(0);
}


/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "quakedef.h"
#include "sys.h"

#ifdef NQ_HACK
#include "client.h"
#include "host.h"

qboolean isDedicated;
#endif

static qboolean noconinput = false;
static qboolean nostdout = false;

// =======================================================================
// General routines
// =======================================================================

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];
    unsigned char *p;

    va_start(argptr, fmt);
    vsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);

    if (nostdout)
	return;

    for (p = (unsigned char *)text; *p; p++) {
	if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
	    printf("[%02x]", *p);
	else
	    putc(*p, stdout);
    }
}

void
Sys_Quit(void)
{
    Host_Shutdown();
    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);
    fflush(stdout);
    exit(0);
}

void
Sys_Init(void)
{
#ifdef USE_X86_ASM
    Sys_SetFPCW();
#endif
}

void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char string[MAX_PRINTMSG];

// change stdin to non blocking
    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);
    fprintf(stderr, "Error: %s\n", string);

    Host_Shutdown();
    exit(1);
}

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int
Sys_FileTime(const char *path)
{
    struct stat buf;

    if (stat(path, &buf) == -1)
	return -1;

    return buf.st_mtime;
}

void
Sys_mkdir(const char *path)
{
    mkdir(path, 0777);
}

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    static char data[MAX_PRINTMSG];
    int fd;

    va_start(argptr, fmt);
    vsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
}

double
Sys_DoubleTime(void)
{
    struct timeval tp;
    struct timezone tzp;
    static int secbase;

    gettimeofday(&tp, &tzp);

    if (!secbase) {
	secbase = tp.tv_sec;
	return tp.tv_usec / 1000000.0;
    }

    return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
}

// =======================================================================
// Sleeps for microseconds
// =======================================================================

#ifdef NQ_HACK
char *
Sys_ConsoleInput(void)
{
    static char text[256];
    int len;
    fd_set fdset;
    struct timeval timeout;

    if (cls.state == ca_dedicated) {
	FD_ZERO(&fdset);
	FD_SET(STDIN_FILENO, &fdset);	// stdin
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if (select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == -1
	    || !FD_ISSET(STDIN_FILENO, &fdset))
	    return NULL;

	len = read(STDIN_FILENO, text, sizeof(text));
	if (len < 1)
	    return NULL;
	text[len - 1] = 0;	// rip off the /n and terminate

	return text;
    }
    return NULL;
}
#endif

#ifndef USE_X86_ASM
void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}
#endif

int
main(int c, const char **v)
{
    double time, oldtime, newtime;
    quakeparms_t parms;
    int j;

    signal(SIGFPE, SIG_IGN);

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(c, v);
    parms.argc = com_argc;
    parms.argv = com_argv;

    parms.memsize = 16 * 1024 * 1024;

    j = COM_CheckParm("-mem");
    if (j)
	parms.memsize = (int)(Q_atof(com_argv[j + 1]) * 1024 * 1024);
    parms.membase = malloc(parms.memsize);
    parms.basedir = stringify(QBASEDIR);

    if (COM_CheckParm("-noconinput"))
	noconinput = true;
    if (COM_CheckParm("-nostdout"))
	nostdout = true;

    // Make stdin non-blocking
    // FIXME - check both return values
    if (!noconinput)
	fcntl(STDIN_FILENO, F_SETFL,
	      fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    if (!nostdout)
#ifdef NQ_HACK
	printf("Quake -- TyrQuake Version %s\n", stringify(TYR_VERSION));
#endif
#ifdef QW_HACK
	printf("QuakeWorld -- TyrQuake Version %s\n", stringify(TYR_VERSION));
#endif

    Sys_Init();
    Host_Init(&parms);

#ifdef NQ_HACK
    oldtime = Sys_DoubleTime() - 0.1;
#endif
#ifdef QW_HACK
    oldtime = Sys_DoubleTime();
#endif
    while (1) {
// find time spent rendering last frame
	newtime = Sys_DoubleTime();
	time = newtime - oldtime;

#ifdef NQ_HACK
	if (cls.state == ca_dedicated) {
	    if (time < sys_ticrate.value) {
		usleep(1);
		continue;	// not time to run a server only tic yet
	    }
	    time = sys_ticrate.value;
	}
	if (time > sys_ticrate.value * 2)
	    oldtime = newtime;
	else
	    oldtime += time;
#endif
#ifdef QW_HACK
	oldtime = newtime;
#endif

	Host_Frame(time);
    }

    return 0;
}


/*
================
Sys_MakeCodeWriteable
================
*/
void
Sys_MakeCodeWriteable(void *start_addr, void *end_addr)
{
    void *addr;
    size_t length;
    intptr_t pagesize;
    int result;

    pagesize = getpagesize();
    addr = (void *)((intptr_t)start_addr & ~(pagesize - 1));
    length = ((byte *)end_addr - (byte *)addr) + pagesize - 1;
    length &= ~(pagesize - 1);
    result = mprotect(addr, length, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (result < 0)
	Sys_Error("Protection change failed");
}

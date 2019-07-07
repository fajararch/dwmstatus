
#define _DEFAULT_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>

#include <X11/Xlib.h>

#define _SYSCLASSDIR "/sys/class/power_supply"
#define _DEFAULT_TZ  "Asia/Jakarta"

/**
 * Kernel 3.4+
 * AC Adapter => /sys/class/power_supply/AC
 * 		plugged in/out see "online"
 * Battery    => /sys/class/power_supply/BATx
 *      current level see "charge_now" or "energy_now"
 *      max level see "charge_full" or "energy_full"
 */
char acad[10];
char battery[10];

static Display *dpy;

int nproc = 1;

/************************ Helper functions ************************/
/**
 * Count the number of processors used by system
 **/
int
count_processor(void)
{
	int count = 0;
	FILE * cpuinfo = fopen("/proc/cpuinfo", "rb");
	char * arg = 0;
	size_t size = 0;
	char * line = NULL;

	while (getdelim(&arg, &size, 0, cpuinfo) != -1)
	{
		line = strtok(arg, "\n");
		while (line != NULL)
		{
			if (!strncmp(line, "processor", strlen("processor")))
				count ++;

			line = strtok(NULL,"\n");
		}
		free(arg);
	}
	fclose(cpuinfo);

	return count;
}

/**
 * Search the instance of battery and power supply
 * on "/sys/class/power_supply"
 **/
void
search_power_supply_dev(void)
{
	DIR *d;
	struct dirent *dp;

	if ((d = opendir(_SYSCLASSDIR)) == NULL) {
		return;
	}

	memset(acad, 0x00, sizeof(acad));
	memset(battery, 0x00, sizeof(battery));

	while ((dp = readdir(d)) != NULL) {
		if (!strncmp(dp->d_name,"BAT", 3) && battery[0]==0x00)
			strncpy(battery, dp->d_name, sizeof(battery)-1);

		if (!strncmp(dp->d_name,"AC", 2) && acad[0]==0x00)
			strncpy(acad, dp->d_name, sizeof(acad)-1);
	}
}

/**
 * Search the instance of network device
 * on /dev
 **/
int
parse_netdev(unsigned long long int *receivedabs, unsigned long long int *sentabs)
{
	char buf[255];
	char *datastart;
	static int bufsize;
	int rval;
	FILE *devfd;
	unsigned long long int receivedacc, sentacc;

	bufsize = 255;
	devfd = fopen("/proc/net/dev", "r");
	rval = 1;

	// Ignore the first two lines of the file
	fgets(buf, bufsize, devfd);
	fgets(buf, bufsize, devfd);

	while (fgets(buf, bufsize, devfd)) {
	    if ((datastart = strstr(buf, "lo:")) == NULL) {
		datastart = strstr(buf, ":");

		// With thanks to the conky project at http://conky.sourceforge.net/
		sscanf(datastart + 1, "%llu  %*d     %*d  %*d  %*d  %*d   %*d        %*d       %llu",\
		       &receivedacc, &sentacc);
		*receivedabs += receivedacc;
		*sentabs += sentacc;
		rval = 0;
	    }
	}

	fclose(devfd);
	return rval;
}

/**
 * Build a string which will be output to the status panel
 **/
char *
smprintf(char *fmt, ...)
{
	va_list fmtargs;
	char *ret;
	int len;

	va_start(fmtargs, fmt);
	len = vsnprintf(NULL, 0, fmt, fmtargs);
	va_end(fmtargs);

	ret = malloc(++len);
	if (ret == NULL) {
		perror("malloc");
		exit(1);
	}

	va_start(fmtargs, fmt);
	vsnprintf(ret, len, fmt, fmtargs);
	va_end(fmtargs);

	return ret;
}

/**
 * Calculate the speed
 **/
void
calculate_speed(char *speedstr, unsigned long long int newval, unsigned long long int oldval)
{
	double speed;
	speed = (newval - oldval) / 1024.0;

	// TODO: coloring
	if (speed > 1024.0) {
	    speed /= 1024.0;
	    sprintf(speedstr, "%.3f MB/s", speed);
	} else {
	    sprintf(speedstr, "%.2f KB/s", speed);
	}
}

void
setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

/************************ DWM Widgets ************************/
/**
 * Get CPU load average and display it to status panel.
 * Color it accordingly.
 *     red    => 70-100 %
 *     yellow => 40-69  %
 *     green  => 0-39   %
 **/
char *
loadavg(void)
{
	int i;
	char   * fmt  = NULL;
	double * cpus = NULL;

	if (fmt == NULL)
		fmt = (char*) malloc(nproc * 8 * sizeof(char));
	if (cpus == NULL)
		cpus = (double*)malloc(nproc * sizeof(double));

	memset(fmt, 0x00, nproc * 5 * sizeof(char));
	memset(cpus, 0x00, nproc * sizeof(double));

	if (getloadavg(cpus, nproc+1) < 0) {
		perror("getloadavg");
		exit(1);
	}

	// TODO: coloring
	sprintf(fmt, "%.2f", cpus[0]);
	for (i=1; i<nproc; i++)
		sprintf(fmt+strlen(fmt), " %.2f", cpus[i]);

	free(cpus);

//	return smprintf(fmt, fmtargs);
	return fmt;
}


/**
 * Get network speed and display it to status panel.
 * Color it accordingly.
 *     red    => 70-100 %
 *     yellow => 40-69  %
 *     green  => 0-39   %
 **/
char *
get_netusage(unsigned long long int *rec, unsigned long long int *sent)
{
	unsigned long long int newrec, newsent;
	newrec = newsent = 0;
	char downspeedstr[15], upspeedstr[15];
	static char retstr[42];
	int retval;

	retval = parse_netdev(&newrec, &newsent);
	if (retval) {
	    fprintf(stdout, "Error when parsing /proc/net/dev file.\n");
	    exit(1);
	}

	calculate_speed(downspeedstr, newrec, *rec);
	calculate_speed(upspeedstr, newsent, *sent);

	sprintf(retstr, "Downstream: %s      Upstream: %s", downspeedstr, upspeedstr);

	*rec = newrec;
	*sent = newsent;
	return retstr;
}


/**
 * Get memory usage and display it to status panel.
 * Color it accordingly.
 *     red    => 70-100 %
 *     yellow => 40-69  %
 *     green  => 0-39   %
 **/
char*
get_memusage(){
	FILE *fd;
    long total, fre, buf, cache;
    double used;
    static char memstr[42];

    fd = fopen("/proc/meminfo", "r");
    fscanf(fd, "MemTotal: %ld kB\nMemFree: %ld kB\nBuffers: %ld kB\nCached: %ld kB\n", &total, &fre, &buf, &cache);
    fclose(fd);

    used = (double)(total - fre) / total;

	// TODO: coloring
    sprintf(memstr, "%.2lf%%", 100*used);

    return memstr;
}


/**
 * Get the current time and display it to status panel.
 * Color it accordingly.
 **/
char *
mktimes(char *fmt)
{
	char buf[129];
	time_t tim;
	struct tm *timtm;

	memset(buf, 0, sizeof(buf));
	tim = time(NULL);
	timtm = localtime(&tim);
	if (timtm == NULL) {
		perror("localtime");
		exit(1);
	}

	if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
		fprintf(stderr, "strftime == 0\n");
		exit(1);
	}

	return smprintf("%s", buf);
}


/**
 * Get the status of power supply and display it to status panel.
 * Color it accordingly.
 *     red    => 0-15   %
 *     yellow => 16-30  %
 *     green  => 31-100 %
 **/
char*
parse_power_supply(void)
{
	char b[PATH_MAX];
	int  alloc = 16;
	FILE *fc, *ff, *fo;
	char *power = NULL;
	long current, full, online;

	power = (char*) malloc(alloc);
	memset(power, 0x00, alloc);

	// We get battery from _SYSCLASSDIR/battery
	if (battery[0] != 0x00)
	{
		snprintf(b, PATH_MAX, "%s/%s/%s", _SYSCLASSDIR, battery, "charge_now");
		fc = fopen(b, "r");
		if (fc == NULL)
		{
			snprintf(b, PATH_MAX, "%s/%s/%s", _SYSCLASSDIR, battery, "energy_now");
			fc = fopen(b, "r");
		}
		snprintf(b, PATH_MAX, "%s/%s/%s", _SYSCLASSDIR, battery, "charge_full");
		ff = fopen(b, "r");
		if (ff == NULL)
		{
			snprintf(b, PATH_MAX, "%s/%s/%s", _SYSCLASSDIR, battery, "energy_full");
			ff = fopen(b, "r");
		}

		current = 0;
		full = 0;
		if (fc != NULL && ff != NULL) {
			fscanf(fc, "%ld", &current);
			fclose(fc);
			fscanf(ff, "%ld", &full);
			fclose(ff);
		}

		// TODO: coloring
		// Prevent div by zero
		if (full)
			sprintf(power, "%.2f %% ", (current * 100.0) / full);
		else
			strcpy(power, "0.0 %% ");
	}

	if (acad[0] != 0x00)
	{
		// TODO: coloring
		snprintf(b, PATH_MAX, "%s/%s/%s", _SYSCLASSDIR, acad, "online");
		fo = fopen(b, "r");

		online = 0;
		if (fo != NULL)
			fscanf(fo, "%ld", &online);

		if (online)
			strcat(power, "P");		// Plugged In
		else
			strcat(power, "U");		// Unplugged

		if (fo) fclose(fo);
	}

	return power;
}

/******************************************************************************/
int
main(void)
{
	char *status;
	char *avgs;
	char *tmutc;
	char *netstats;
	char *mem;
	char *power;

	static unsigned long long int rec, sent;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwmstatus: cannot open display.\n");
		return 1;
	}

	//settz();
	nproc = count_processor();
	search_power_supply_dev();
	parse_netdev(&rec, &sent);
	for (;;sleep(1)) {
		avgs = loadavg();
		power = parse_power_supply();
		tmutc = mktimes(" %a %d %b %Y      %H:%M:%S %Z");
		netstats = get_netusage(&rec, &sent);
		mem = get_memusage();

		// Display to main status panel
		status = smprintf("%s; Battery: %s      %s      Memory: %s      CPU: %s",
					tmutc, power, netstats, mem, avgs);
		setstatus(status);
		free(status);
		free(tmutc);
		free(power);
	}

	XCloseDisplay(dpy);

	return 0;
}


/* A status bar for dwm. */

/* Feature-test macro for nanosleep(2). */
#if ! (_POSIX_C_SOURCE >= 199309L)
#define _POSIX_C_SOURCE 199309L
#endif /* _POSIX_C_SOURCE */

/* Feature-test macro for snprintf(3). */
#if _BSD_SOURCE
#define _BSD_SOURCE
#endif /* _BSD_SOURCE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <X11/Xlib.h>

/* Buffer size for status string. */
const static size_t  STATUS_LEN = 256;

/* Percentage to display if an error occurs in reading cpu usage. */
const static float  ERROR_CPU_PER = 3133.7;

/* MONITOR_SLEEP_INTERVAL must be a multiple of STATUS_SLEEP_INTERVAL. */
const static struct timespec STATUS_SLEEP_INTERVAL  = { (time_t) 1, 0L };
const static struct timespec MONITOR_SLEEP_INTERVAL = { (time_t) 4, 0L };

static Display *dpy;

void setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

char *getdatetime(void)
{
	char *buf;
	time_t result;
	struct tm *resulttm;

	if((buf = malloc(sizeof(char)*65)) == NULL) {
		fprintf(stderr, "Cannot allocate memory for buf.\n");
		exit(1);
	}
	result = time(NULL);
	resulttm = localtime(&result);
	if(resulttm == NULL) {
		fprintf(stderr, "Error getting localtime.\n");
		exit(1);
	}
	if(!strftime(buf, sizeof(char)*65-1, "%a %b %d %H:%M:%S", resulttm)) {
		fprintf(stderr, "strftime is 0.\n");
		exit(1);
	}

	return buf;
}

float getbattery(void)
{
	FILE *fd;
	int charge_now, charge_full, energy_now, energy_full, voltage_now;

	if ((fd = fopen("/sys/class/power_supply/BAT0/energy_now", "r"))
			!= NULL) {
		fscanf(fd, "%d", &energy_now);
		fclose(fd);

		fd = fopen("/sys/class/power_supply/BAT0/energy_full", "r");
		if(fd == NULL) {
			fprintf(stderr, "Error opening energy_full.\n");
			return -1;
		}
		fscanf(fd, "%d", &energy_full);
		fclose(fd);

		fd = fopen("/sys/class/power_supply/BAT0/voltage_now", "r");
		if(fd == NULL) {
			fprintf(stderr, "Error opening voltage_now.\n");
			return -1;
		}
		fscanf(fd, "%d", &voltage_now);
		fclose(fd);

		return ((float)energy_now * 1000 / (float)voltage_now) * 100 /
			((float)energy_full * 1000 / (float)voltage_now);

	} else if ((fd = fopen("/sys/class/power_supply/BAT1/charge_now", "r"))
			!= NULL) {
		fscanf(fd, "%d", &charge_now);
		fclose(fd);

		fd = fopen("/sys/class/power_supply/BAT1/charge_full", "r");
		if(fd == NULL) {
			fprintf(stderr, "Error opening charge_full.\n");
			return -1;
		}
		fscanf(fd, "%d", &charge_full);
		fclose(fd);

		return (((float) charge_now / (float) charge_full) * 100.0);
	} else {
		return -1.0;
	}
}

float ramusage(void)
{
	FILE *fd;
	char *buf;
	int total, active, buflen;

	total  = active = -1;
	buflen = 512;

	if (!(fd = fopen("/proc/meminfo", "r"))) {
		fprintf(stderr, "Error opening /proc/meminfo.\n");
		return -1;
	}

	buf = (char *) malloc(sizeof(char) * buflen);
	while (total == -1) {
		fgets(buf, buflen, fd);
		sscanf(buf, "MemTotal: %d kB\n", &total);
	}
	while (active == -1) {
		fgets(buf, buflen, fd);
		sscanf(buf, "Active: %d kB\n", &active);
	}

	fclose(fd);
	free(buf);

	return ((float) active / (float) total) * 100.0;
}

float cpuusage()
{
	FILE *fd;
	int user_jiff, nice_jiff, sys_jiff, idle_jiff, new_user_jiff,
		new_nice_jiff, new_sys_jiff, new_idle_jiff;

	/* Example line from `/proc/stat`:
	 * cpu  234646 66 57678 21381296 43506 2 362 0 0 0
	 */

	if (!(fd = fopen("/proc/stat", "r"))) {
		fprintf(stderr, "Error opening /proc/stat.\n");
		return ERROR_CPU_PER;
	}
	if (fscanf(fd, "cpu  %d %d %d %d", &user_jiff, &nice_jiff,
				&sys_jiff, &idle_jiff) != 4)
		return ERROR_CPU_PER;

	nanosleep(&MONITOR_SLEEP_INTERVAL, NULL);

	if (!(fd = freopen("/proc/stat", "r", fd))) {
		fprintf(stderr, "Error opening /proc/stat.\n");
		return 7331.3;
	}
	if (fscanf(fd, "cpu  %d %d %d %d", &new_user_jiff, &new_nice_jiff,
				&new_sys_jiff, &new_idle_jiff) != 4)
		return 7331.3;

	fclose(fd);

	/* Calculate percentage from diffs in jiffies between work and idle. */
	return ((float) ((user_jiff + nice_jiff + sys_jiff)
				- (new_user_jiff + new_nice_jiff + new_sys_jiff))
			/ (float) (idle_jiff - new_idle_jiff)) * 100.0;
}

int main(void)
{
	char *status, *datetime;
	float bat, ram;
	int pipefd[2];
	pid_t pid;
	union fbuf {
		float f;
		void *b;
	};

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "Cannot open display.\n");
		return 1;
	}

	if ((status = (char *) malloc(sizeof(char) * STATUS_LEN)) == NULL) {
		fprintf(stderr, "Cannot allocate memory for status string.\n");
		return 1;
	}

	if (pipe(pipefd) == -1) {
		perror("cpu monitor pipe");
		exit(1);
	}
	pid = fork();

	/* Fork off CPU usage monitor. */
	if (pid == -1) { /* error */
		fputs("Failed to fork cpu percentage monitor.\n", stderr);
		exit(1);
	} else if (pid == 0) { /* child */
		close(pipefd[0]);

		union fbuf cbuf = { 0 };
		for (;;) { /* forever */
			cbuf.f = cpuusage();

			if (write(pipefd[1], &(cbuf.b), sizeof(cbuf.f)) == -1) {
				perror("cpu monitor write");
				exit(1);
			}
		}
	} else { /* parent */
		close(pipefd[1]);
	}

	union fbuf cbuf = { 0 };
	for (time_t status_sleeps = (time_t) 0 ;; status_sleeps++,
			nanosleep(&STATUS_SLEEP_INTERVAL, NULL)) {
		ram      = ramusage();
		datetime = getdatetime();
		bat      = getbattery();

		if (status_sleeps == MONITOR_SLEEP_INTERVAL.tv_sec
				/ STATUS_SLEEP_INTERVAL.tv_sec) {
			if (read(pipefd[0], &(cbuf.b), sizeof(cbuf.f)) == -1) {
				perror("cpu monitor read");
				exit(1);
			}
			status_sleeps = 0;
		}

		if (bat != -1.0)
			snprintf(status, STATUS_LEN,
					"[ %.1f%% cpu | %.1f%% mem ]  [ %.1f%% | %s ]",
					cbuf.f, ram, bat, datetime);
		else
			snprintf(status, STATUS_LEN,
					"[ %.1f%% cpu | %.1f%% mem ]  [ %s ]",
					cbuf.f, ram, datetime);

		free(datetime);
		setstatus(status);
	}

	free(status);
	XCloseDisplay(dpy);

	return 0;
}


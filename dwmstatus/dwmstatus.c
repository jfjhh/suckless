/*
 * A status bar for dwm.
 * Many changes were made by Alex Striff <alex.striff1@gmail.com>.
 */

/*
 * MIT/X Consortium License
 *
 * © 2011-12 Christoph Lohmann <20h@r-36.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Feature-test macro for nanosleep(2). */
#if ! (_POSIX_C_SOURCE >= 199309L)
#define _POSIX_C_SOURCE 199309L
#endif /* _POSIX_C_SOURCE */

/* Feature-test macro for snprintf(3). */
#if _BSD_SOURCE
#define _BSD_SOURCE
#endif /* _BSD_SOURCE */

/* Feature-test macro for dup2(2). */
#if _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

/* Feature-test macro for strndup(3). */
#if ! ( _XOPEN_SOURCE >= 700 )
#define _XOPEN_SOURCE 700
#endif /* _XOPEN_SOURCE */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <alloca.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <X11/Xlib.h>
#include <alsa/asoundlib.h>

/* Directories for system info. */
#define MEMINFO	"/proc/meminfo"
#define STAT	"/proc/stat"
#define THERMAL	"/sys/class/thermal/thermal_zone0"

/* Buffer size for status string. */
const static size_t  STATUS_LEN   = 1024;

/* Buffer size for thermal trip point type. */
const static size_t  TRIP_LEN     = 16;

/* Percentage to display if an error occurs in reading cpu usage. */
const static float  ERROR_CPU_PER = 3133.7;

/* MONITOR_SLEEP_INTERVAL must be a multiple of STATUS_SLEEP_INTERVAL. */
const static struct timespec STATUS_SLEEP_INTERVAL  = { (time_t) 1, 0L };
const static struct timespec MONITOR_SLEEP_INTERVAL = { (time_t) 2, 0L };

/* Variables that will be freed by the signal handler. */
static Display      *dpy;

static int vol_left, vol_right;

void setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

static inline void protect_free(void *x)
{
	free(x);
	x = NULL;
}

char *getcmustrack(void)
{
	char *track, *tofree, *cfree;
	int e, i;
	FILE *file;
	size_t n;

	if (!(file = popen("cmus-remote -Q", "r")))
		return NULL;

	e = i = 0;
	while (e != 1) {
		n = 0;
		tofree = NULL;
		if (getline(&tofree, &n, file) == -1) {
			pclose(file);
			free(tofree);
			return NULL;
		}
		if (strstr(tofree, "file "))
			e = 1;
		else
			free(tofree);
		if (e == EOF) {
			pclose(file);
			free(tofree);
			fputs("EOF for cmus-remote 'file' field read!\n", stderr);
			return NULL;
		}
	}

	char *ext = strrchr(tofree, '.');
	size_t end = ((!ext) ? strlen(tofree) : ext - tofree) - strlen("file ");
	cfree  = strndup(tofree + strlen("file "), end + 1);
	track  = basename(cfree);
	track  = strdup(track);
	protect_free(cfree);
	track[strlen(track) - 1] = '\0';

	free(tofree);
	e      = 0;
	n      = 0;

	while (getline(&tofree, &n, file) != -1) {
		/* Search for metadata name; it is preferred over of file name. */
		if (tofree && strstr(tofree, "tag title ")) {
			free(track);
			track = strdup(tofree + strlen("tag title "));
			track[strlen(track) - 1] = '\0';
		}

		/* Search for volume(s). */
		sscanf(tofree, "set vol_left %d", &vol_left);
		sscanf(tofree, "set vol_right %d", &vol_right);

		protect_free(tofree);
		n = 0;
	}
	pclose(file);
	free(tofree);
	return track;
}

char *getdatetime(void)
{
	char *buf;
	time_t result;
	struct tm *resulttm;

	if((buf = malloc(sizeof(char)*65)) == NULL) {
		fputs("Cannot allocate memory for buf.\n", stderr);
		return NULL;
	}
	result   = time(NULL);
	resulttm = localtime(&result);
	if(resulttm == NULL) {
		fputs("Error getting localtime.\n", stderr);
		return NULL;
	}
	if(!strftime(buf, sizeof(char)*65-1, "%a %b %d %H:%M:%S", resulttm)) {
		fputs("strftime is 0.\n", stderr);
		return NULL;
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
			fputs("Error opening energy_full.\n", stderr);
			return -1;
		}
		fscanf(fd, "%d", &energy_full);
		fclose(fd);

		fd = fopen("/sys/class/power_supply/BAT0/voltage_now", "r");
		if(fd == NULL) {
			fputs("Error opening voltage_now.\n", stderr);
			return -1;
		}
		fscanf(fd, "%d", &voltage_now);
		fclose(fd);

		return ((float) energy_now * 1000 / (float) voltage_now) * 100 /
			((float) energy_full * 1000 / (float) voltage_now);

	} else if ((fd = fopen("/sys/class/power_supply/BAT1/charge_now", "r"))
			!= NULL) {
		fscanf(fd, "%d", &charge_now);
		fclose(fd);

		fd = fopen("/sys/class/power_supply/BAT1/charge_full", "r");
		if(fd == NULL) {
			fputs("Error opening charge_full.\n", stderr);
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

	if (!(fd = fopen(MEMINFO, "r"))) {
		fputs("Error opening " MEMINFO ".\n", stderr);
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

bool getthermal(float *cur, float *trip, char *type)
{
	FILE *fd;
	int temp;

	if (!(fd = fopen(THERMAL "/temp", "r"))) {
		fputs("Error opening " THERMAL ".\n", stderr);
		return false;
	} else {
		if (!fscanf(fd, "%d", &temp))
			return false;
		*cur = (float) temp / 1000.0;
	}

	if (!(fd = freopen(THERMAL "/trip_point_0_temp", "r", fd))) {
		fputs("Error opening " THERMAL ".\n", stderr);
		return false;
	} else {
		if (!fscanf(fd, "%d", &temp))
			return false;
		*trip = (float) temp / 1000.0;
	}

	if (!(fd = freopen(THERMAL "/trip_point_0_type", "r", fd))) {
		fputs("Error opening " THERMAL ".\n", stderr);
		return false;
	} else {
		if (!fscanf(fd, "%s", type))
			return false;
	}

	fclose(fd);
	return true;
}

float cpuusage()
{
	FILE *fd;
	int old_work, old_total, new_work, new_total, jiff;
	old_work = old_total = new_work = new_total = jiff = 0;

	if (!(fd = fopen(STAT, "r"))) {
		fputs("Error opening " STAT ".\n", stderr);
		return ERROR_CPU_PER;
	}
	if (fseek(fd, 4L, SEEK_CUR) == -1)
		return ERROR_CPU_PER;
	
	for (size_t i = 0; fscanf(fd, " %d", &jiff) == 1; i++) {
		if (i < 3)
			old_work += jiff;
		old_total += jiff;
	}

	nanosleep(&MONITOR_SLEEP_INTERVAL, NULL);

	if (!(fd = freopen(STAT, "r", fd))) {
		fputs("Error opening " STAT ".\n", stderr);
		return ERROR_CPU_PER;
	}
	if (fseek(fd, 4L, SEEK_CUR) == -1)
		return ERROR_CPU_PER;

	for (size_t i = 0; fscanf(fd, " %d", &jiff) == 1; i++) {
		if (i < 3)
			new_work += jiff;
		new_total += jiff;
	}

	fclose(fd);

	return 100.0 * ((float) (new_work - old_work))
		/ ((float) (new_total - old_total));
}

float getvol()
{
	float perc = 0.0;
	long min, max, volume;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	const char *card = "default";
	const char *selem_name = "Master";
	int mute = -1;

	snd_mixer_open(&handle, 0);
	snd_mixer_attach(handle, card);
	snd_mixer_selem_register(handle, NULL, NULL);
	snd_mixer_load(handle);

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, selem_name);
	snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

	if (snd_mixer_selem_has_playback_switch(elem)
			&& !snd_mixer_selem_get_playback_switch(elem, 0, &mute)
			&& snd_mixer_selem_has_playback_volume(elem)
			&& !snd_mixer_selem_get_playback_volume_range(elem, &min, &max)
			&& !snd_mixer_selem_get_playback_volume(elem, 0, &volume)) {
		perc = 100.0 * ((float) volume - (float) min) /
			((float) max - (float) min);
		if (mute)
			perc = -perc;
	}

	snd_mixer_close(handle);
	return perc;
}

static void signal_handler(int signum)
{
	XFlush(dpy);

	kill(getpid(), signum);
	_exit(EXIT_FAILURE);
}

int main(void)
{
	struct sigaction sa;
	char *status, *thermal_type, *datetime, *track;
	float bat, ram, cur_temp, trip_temp, alsa_vol;
	int   pipefd[2], cmus_vol;
	pid_t pid;
	union fbuf {
		float f;
		void *b;
	};
	status   = datetime = thermal_type = track = NULL;
	cmus_vol = -1;

	/* Handle the signals. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	if (sigfillset(&sa.sa_mask) < 0) {
		perror("sigfillset()");
		return 1;
	}
	if ((sigaction(SIGHUP, &sa, NULL) < 0)
			|| (sigaction(SIGINT, &sa, NULL) < 0)
			|| (sigaction(SIGTERM, &sa, NULL) < 0)) {
		perror("sigaction()");
		return 1;
	}

	if (!(dpy = XOpenDisplay(NULL))) {
		fputs("Cannot open display.\n", stderr);
		return 1;
	}
	if (!(status = calloc(sizeof(char), STATUS_LEN))) {
		fputs("Cannot allocate memory for status string.\n", stderr);
		XCloseDisplay(dpy);
		return 1;
	}
	if (!(thermal_type = calloc(sizeof(char), TRIP_LEN))) {
		fputs("Cannot allocate memory for thermal_type string.\n", stderr);
		free(status);
		XCloseDisplay(dpy);
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
		size_t status_chars = 0;
		bool   temp_ok;

		track    = getcmustrack();
		cmus_vol = (vol_left + vol_right) / 2;
		ram      = ramusage();
		datetime = getdatetime();
		bat      = getbattery();
		temp_ok  = getthermal(&cur_temp, &trip_temp, thermal_type);
		alsa_vol = getvol();

		if (status_sleeps == MONITOR_SLEEP_INTERVAL.tv_sec
				/ STATUS_SLEEP_INTERVAL.tv_sec) {
			if (read(pipefd[0], &(cbuf.b), sizeof(cbuf.f)) == -1) {
				perror("cpu monitor read");
				exit(1);
			}
			status_sleeps = 0;
		}

		/* Add track name to status. */
		if (track) {
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					"<span color='#859900'>%s</span> ", track);

			/* Add cmus volume to status. */
			if (cmus_vol == -1)
				status_chars += snprintf(status + status_chars,
						STATUS_LEN - status_chars,
						"<span color='#D33682'>\u266a:M</span> ");
			else if (cmus_vol >= 0)
				status_chars += snprintf(status + status_chars,
						STATUS_LEN - status_chars,
						"<span color='#D33682'>\u266a:%d%%</span> ", cmus_vol);
		}

		/* Add alsa volume and mute status to status. (alsa_vol 0 is error). */
		if (alsa_vol < 0)
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					"<span color='#D33682'>A:%d%%</span>  ", -(int) alsa_vol);
		else if (alsa_vol > 0)
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					"<span color='#D33682'>M:%d%%</span>  ", (int) alsa_vol);
		else
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					" ");

		/* Add cyan color tag to status. */
		status_chars += snprintf(status + status_chars,
				STATUS_LEN - status_chars,
				"<span color='#2AA198'>");

		/* Add temperature to status. */
		if (temp_ok)
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					"%3.1fC cur, %3.1fC %s, ",
					cur_temp, trip_temp, thermal_type);

		/* Add cpu and memory usage to status. */
		status_chars += snprintf(status + status_chars,
				STATUS_LEN - status_chars,
				"%.1f%% cpu, %.1f%% mem",
				cbuf.f, ram);

		/* Add battery to status. */
		if (bat != -1.0)
			status_chars += snprintf(status + status_chars,
					STATUS_LEN - status_chars,
					", %.1f%%", bat);

		/* Add date and time to status. */
		status_chars += snprintf(status + status_chars,
				STATUS_LEN - status_chars,
				"</span>  <span color='#268BD2'>%s</span> ", datetime);

		protect_free(datetime);
		protect_free(track);
		setstatus(status);
	}

	protect_free(status);
	protect_free(thermal_type);
	XCloseDisplay(dpy);

	return 0;
}


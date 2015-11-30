/* A status bar for dwm. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <X11/Xlib.h>

static Display *dpy;

void setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

float getfreq(char *file)
{
	FILE *fd;
	char *freq;
	float ret;

	freq = malloc(10);
	fd = fopen(file, "r");
	if(fd == NULL) {
		fprintf(stderr, "Cannot open '%s' for reading.\n", file);
		exit(1);
	}

	fgets(freq, 10, fd);
	fclose(fd);

	ret = atof(freq) / 1e6;
	free(freq);
	return ret;
}

char *getcpu(void)
{
	FILE *fd;
	char *buf, *buf_part, *scaling_cur_freq;
	size_t buf_len, buf_part_len, scaling_cur_freq_len;
	int min, max, cur;
	float *freqs;

	fd = fopen("/sys/devices/system/cpu/online", "r");
	if (fd == NULL) {
		fprintf(stderr, "Error getting the number of cpus online.\n");
		exit(1);
	}

	fscanf(fd, "%d-%d", &min, &max);
	fclose(fd);

	if (max >= 100) {
		fprintf(stderr, "%d cores!? Too many!\n", max);
		exit(1);
	}

	if ((freqs = (float *) malloc(sizeof(float) * (max + 1))) == NULL) {
		fprintf(stderr, "Cannot allocate memory for cpu frequencies.\n");
		exit(1);
	}

	// 6 chars for each core, plus the terminating '\0' byte.
	buf_part_len = 7;
	if ((buf_part = (char *) malloc(sizeof(char) * buf_part_len)) == NULL) {
		fprintf(stderr, "Cannot allocate memory for buf_part.\n");
		exit(1);
	}
	memset(buf_part, '\0', buf_part_len);

	// buf_part_len chars for each core; take 2 chars to not have ", " at end.
	buf_len = (((max + 1) * (buf_part_len - 1)) - 2 ) + 1;
	if ((buf = (char *) malloc(sizeof(char) * buf_len)) == NULL) {
		fprintf(stderr, "Cannot allocate memory for buf.\n");
		exit(1);
	}
	memset(buf, '\0', buf_len);

	for (cur = min; cur <= max; cur++) {
		scaling_cur_freq_len =
			strlen("/sys/devices/system/cpu/cpu/cpufreq/scaling_cur_freq") +
			1 + ((cur >= 10) ? 2 : 1);
		scaling_cur_freq = (char *) malloc(sizeof(char) * scaling_cur_freq_len);

		snprintf(scaling_cur_freq, scaling_cur_freq_len,
				"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cur);

		freqs[cur] = getfreq(scaling_cur_freq);

		if (cur != max)
			snprintf(buf_part, buf_part_len, "%0.2f, ", freqs[cur]);
		else
			snprintf(buf_part, buf_part_len - 2, "%0.2f", freqs[cur]);

		strncat(buf, buf_part, buf_len + buf_part_len - 1);
		free(scaling_cur_freq);
	}

	free(freqs);
	free(buf_part);
	return buf;
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

float cpuusage(void)
{
	FILE *fd;
	int user_jiff, nice_jiff, sys_jiff;

	if (!(fd = fopen("/proc/stat", "r"))) {
		fprintf(stderr, "Error opening /proc/stat.\n");
		return -1;
	}

	// cpu  234646 66 57678 21381296 43506 2 362 0 0 0
	fscanf(fd, "cpu  %d %d %d", &user_jiff, &nice_jiff, &sys_jiff);
}

int main(void)
{
	char *status, *datetime, *cpu_str;
	float bat, ram, cpu;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "Cannot open display.\n");
		return 1;
	}

	if ((status = (char *) malloc(sizeof(char) * 200)) == NULL) {
		fprintf(stderr, "Cannot allocate memory for status string.\n");
		return 1;
	}

	for (;; sleep(1)) {
		cpu_str = getcpu();
		ram = ramusage();
		cpu = cpuusage();
		datetime = getdatetime();
		bat = getbattery();

		// if (bat != -1.0)
		// 	snprintf(status, 200, "[ %s | %0.1f%% ]  [ %0.1f%% | %s ]",
		// 			cpu_str, ram, bat, datetime);
		// else
		// 	snprintf(status, 200, "[ %s | %0.1f%% ]  [ %s ]",
		// 			cpu_str, ram, datetime);

		if (bat != -1.0)
			snprintf(status, 200, "[ %0.1f%% | %0.1f%% ]  [ %0.1f%% | %s ]",
					cpu, ram, bat, datetime);
		else
			snprintf(status, 200, "[ %0.1f%% | %0.1f%% ]  [ %s ]",
					cpu, ram, datetime);

		free(cpu_str);
		free(datetime);
		setstatus(status);
	}

	free(status);
	XCloseDisplay(dpy);

	return 0;
}


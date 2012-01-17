/******************************************************************************
 * Parse DVB tables
 * According with:
 *	ETSI EN 301 192 V1.5.1 (2009-11)
 *	ISO/IEC 13818-1:2007
 *	ETSI EN 300 468 V1.11.1 (2010-04)
 *****************************************************************************/

#include "libscan.h"
#include "dvb_frontend.h"
#include "descriptors.h"
#include "parse_string.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>

static void parse_pat(struct dvb_descriptors *dvb_desc,
		      const unsigned char *buf, int *section_length,
		      int id, int version)
{
	int service_id, pmt_pid;
	int n;

	dvb_desc->pat_table.ts_id = id;
	dvb_desc->pat_table.version = version;

	n = dvb_desc->pat_table.pid_table_len;
	dvb_desc->pat_table.pid_table = realloc(dvb_desc->pat_table.pid_table,
				sizeof(*dvb_desc->pat_table.pid_table) *
				(n + (*section_length / 4)));

	while (*section_length > 3) {
		service_id = (buf[0] << 8) | buf[1];
		pmt_pid = ((buf[2] & 0x1f) << 8) | buf[3];

		memset(&dvb_desc->pat_table.pid_table[n], 0,
		       sizeof(dvb_desc->pat_table.pid_table[n]));

		dvb_desc->pat_table.pid_table[n].service_id = service_id;
		dvb_desc->pat_table.pid_table[n].pid = pmt_pid;

		if (dvb_desc->verbose)
			printf("#%d, service_id 0x%04x, pmt_pid 0x%04x\n", n, service_id, pmt_pid);

		buf += 4;
		*section_length -= 4;
		n++;
	}
	dvb_desc->pat_table.pid_table_len = n;
}

static void add_vpid(struct pid_table *pid_table, uint16_t pid, int verbose)
{
	int i;

	if (verbose)
		printf("video pid 0x%04x\n", pid);
	i = pid_table->video_pid_len;
	pid_table->video_pid = realloc(pid_table->video_pid,
		sizeof(*pid_table->video_pid) * ++pid_table->video_pid_len);
	pid_table->video_pid[i] = pid;
}

static void add_apid(struct pid_table *pid_table, uint16_t pid, int verbose)
{
	int i;

	if (verbose)
		printf("audio pid 0x%04x\n", pid);
	i = pid_table->audio_pid_len;
	pid_table->audio_pid = realloc(pid_table->audio_pid,
		sizeof(*pid_table->audio_pid) * ++pid_table->audio_pid_len);
	pid_table->audio_pid[i] = pid;
}

static void add_otherpid(struct pid_table *pid_table,
			 uint8_t type, uint16_t pid, int verbose)
{
	int i;

	if (verbose)
		printf("pid type 0x%02x: 0x%04x\n", type, pid);
	i = pid_table->other_el_pid_len;
	pid_table->other_el_pid = realloc(pid_table->other_el_pid,
		sizeof(*pid_table->other_el_pid) *
		++pid_table->other_el_pid_len);

	pid_table->other_el_pid[i].type = type;
	pid_table->other_el_pid[i].pid = pid;
}

static void parse_pmt(struct dvb_descriptors *dvb_desc,
		      const unsigned char *buf, int *section_length,
		      int id, int version,
		      struct pid_table *pid_table)
{
	struct pmt_table *pmt_table = &pid_table->pmt_table;
	uint16_t len, pid;

	pmt_table->program_number = id;
	pmt_table->version = version;

        pmt_table->pcr_pid = ((buf[0] & 0x1f) << 8) | buf[1];
        len = ((buf[2] & 0x0f) << 8) | buf[3];

	if (dvb_desc->verbose)
		printf("PN 0x%04x, version %d, PCR ID 0x%04x, len %d\n",
			pmt_table->program_number, pmt_table->version,
			pmt_table->pcr_pid, len);

	parse_descriptor(PMT, dvb_desc, &buf[4], len);

	buf += 4 + len;
	*section_length -= 4 + len;

	while (*section_length >= 5) {
		len = ((buf[3] & 0x0f) << 8) | buf[4];
		pid = ((buf[1] & 0x1f) << 8) | buf[2];

		switch (buf[0]) {
		case 0x01: /* ISO/IEC 11172-2 Video */
		case 0x02: /* H.262, ISO/IEC 13818-2 or ISO/IEC 11172-2 video */
			add_vpid(pid_table, pid, dvb_desc->verbose);
			break;
		case 0x1b: /* H.264 AVC */
			add_vpid(pid_table, pid, dvb_desc->verbose);
			break;
		case 0x03: /* ISO/IEC 11172-3 Audio */
		case 0x04: /* ISO/IEC 13818-3 Audio */
		case 0x0f: /* ISO/IEC 13818-7 Audio with ADTS (AAC) */
		case 0x11: /* ISO/IEC 14496-3 Audio with the LATM */
		case 0x81: /* user private - in general ATSC Dolby - AC-3 */
			add_apid(pid_table, pid, dvb_desc->verbose);
			break;
		case 0x05: /* private sections */
		case 0x06: /* private data */
			/*
			 * Those can be used by sub-titling, teletext and/or
			 * DVB AC-3. So, need to seek for the AC-3 descriptors
			 */
			if (has_descriptor(dvb_desc, AC_3_descriptor, &buf[5], len) |
			    has_descriptor(dvb_desc, enhanced_AC_3_descriptor, &buf[5], len))
				add_apid(pid_table, pid, dvb_desc->verbose);
			else
				add_otherpid(pid_table, buf[0], pid,
					     dvb_desc->verbose);

			break;
		default:
			add_otherpid(pid_table, buf[0], pid, dvb_desc->verbose);
		};

		parse_descriptor(PMT, dvb_desc, &buf[5], len);
		buf += len + 5;
		*section_length -= len + 5;
		dvb_desc->cur_pmt++;
	};
}

static void parse_nit(struct dvb_descriptors *dvb_desc,
		      const unsigned char *buf, int *section_length,
		      int id, int version)
{
	struct nit_table *nit_table = &dvb_desc->nit_table;
	int len, n;

	nit_table->network_id	 = id;
	nit_table->version = version;

	len = ((buf[0] & 0x0f) << 8) | buf[1];

	if (*section_length < len + 4) {
		fprintf(stderr, "NIT section too short for Network ID 0x%04x", id);
		return;
	}

	parse_descriptor(NIT, dvb_desc, &buf[2], len);

	*section_length -= len + 4;
	buf += len + 4;

	n = nit_table->tr_table_len;
	while (*section_length > 6) {
		nit_table->tr_table = realloc(nit_table->tr_table,
					sizeof(*nit_table->tr_table) * (n + 1));
		memset(&nit_table->tr_table[n], 0,
		       sizeof(nit_table->tr_table[n]));
		nit_table->tr_table[n].tr_id = (buf[0] << 8) | buf[1];

		len = ((buf[4] & 0x0f) << 8) | buf[5];
		if (*section_length < len + 4 && len > 0) {
			fprintf(stderr, "NIT section too short for Network ID 0x%04x, transport stream ID 0x%04x",
			       id, nit_table->tr_table[n].tr_id);
			break;
		} else if (len) {
			if (dvb_desc->verbose)
				printf("Transport stream #%d ID 0x%04x, len %d\n",
					n, nit_table->tr_table[n].tr_id, len);

			parse_descriptor(NIT, dvb_desc, &buf[6], len);
		}

		n++;
		dvb_desc->cur_ts++;
		*section_length -= len + 6;
		buf += len + 6;
	}
	nit_table->tr_table_len = n;
}

static void parse_sdt(struct dvb_descriptors *dvb_desc,
		      const unsigned char *buf, int *section_length,
		      int id, int version)
{
	struct sdt_table *sdt_table = &dvb_desc->sdt_table;
	int len, n;

	sdt_table->ts_id = id;
	sdt_table->version = version;

	buf += 3;
	*section_length -= 3;

	n = sdt_table->service_table_len;
	while (*section_length > 4) {
		sdt_table->service_table = realloc(sdt_table->service_table,
				sizeof(*sdt_table->service_table) * (n + 1));
		memset(&sdt_table->service_table[n], 0,
		       sizeof(sdt_table->service_table[n]));
		sdt_table->service_table[n].service_id = (buf[0] << 8) | buf[1];
		len = ((buf[3] & 0x0f) << 8) | buf[4];
		sdt_table->service_table[n].running = (buf[3] >> 5) & 0x7;
		sdt_table->service_table[n].scrambled = (buf[3] >> 4) & 1;

		if (*section_length < len && len > 0) {
			fprintf(stderr, "SDT section too short for Service ID 0x%04x\n",
			       sdt_table->service_table[n].service_id);
		} else if (len) {
			if (dvb_desc->verbose)
				printf("Service #%d ID 0x%04x, running %d, scrambled %d\n",
				n,
				sdt_table->service_table[n].service_id,
				sdt_table->service_table[n].running,
				sdt_table->service_table[n].scrambled);

			parse_descriptor(SDT, dvb_desc, &buf[5], len);
		}

		n++;
		*section_length -= len + 5;
		buf += len + 5;
		dvb_desc->cur_service++;
	}
	sdt_table->service_table_len = n;
}

static void hexdump(const unsigned char *buf, int len)
{
	int i;

	printf("size %d", len);
	for (i = 0; i < len; i++) {
		if (!(i % 16))
			printf("\n\t");
		printf("%02x ", (uint8_t) *(buf + i));
	}
	printf("\n");
}

static int poll(int filedes, unsigned int seconds)
{
	fd_set set;
	struct timeval timeout;

	/* Initialize the file descriptor set. */
	FD_ZERO (&set);
	FD_SET (filedes, &set);

	/* Initialize the timeout data structure. */
	timeout.tv_sec = seconds;
	timeout.tv_usec = 0;

	/* `select' returns 0 if timeout, 1 if input available, -1 if error. */
	return TEMP_FAILURE_RETRY (select (FD_SETSIZE,
						&set, NULL, NULL,
						&timeout));
}


static int read_section(int dmx_fd, struct dvb_descriptors *dvb_desc,
			uint16_t pid, unsigned char table, void *ptr,
			unsigned timeout)
{
	int count;
	int section_length, table_id, id, version, next = 0;
	unsigned char buf[4096];
	unsigned char *p;
	struct dmx_sct_filter_params f;

	memset(&f, 0, sizeof(f));
	f.pid = pid;
	f.filter.filter[0] = table;
	f.filter.mask[0] = 0xff;
	f.timeout = 0;
	f.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
	if (ioctl(dmx_fd, DMX_SET_FILTER, &f) == -1) {
		perror("ioctl DMX_SET_FILTER failed");
		return -1;
	}

	do {
		do {
			count = poll(dmx_fd, timeout);
			if (count > 0)
				count = read(dmx_fd, buf, sizeof(buf));
		} while (count < 0 && errno == EOVERFLOW);
		if (!count)
			return -1;
		if (count < 0) {
			perror("read_sections: read error");
			return -2;
		}

		p = buf;

		if (count < 3)
			continue;

		table_id = *p;
		section_length = ((p[1] & 0x0f) << 8) | p[2];

		if (count != section_length + 3)
			continue;


		id = (buf[3] << 8) | buf[4];
		version = (buf[5] >> 1) & 0x1f;
		next = (buf[6] == buf[7]) ? 0 : 1;

		if (dvb_desc->verbose) {
			printf("PID 0x%04x, TableID 0x%02x ID=0x%04x, version %d, ",
			pid, table_id, id, version);
			hexdump(buf, count);
			printf("\tsection_length = %d ", section_length);
			printf("section %d, last section %d\n", buf[6], buf[7]);
		}

		p += 8;
		section_length -= 8;

		switch (table_id) {
		case 0x00:	/* PAT */
			parse_pat(dvb_desc, p, &section_length,
					   id, version);
			break;
		case 0x02:	/* PMT */
			parse_pmt(dvb_desc, p, &section_length,
					   id, version, ptr);
			break;
		case 0x40:	/* NIT */
		case 0x41:	/* NIT other */
			parse_nit(dvb_desc, p, &section_length,
					   id, version);
			break;
		case 0x42:	/* SAT */
		case 0x46:	/* SAT other */
			parse_sdt(dvb_desc, p, &section_length,
					   id, version);
			break;
		}
	} while (next);

	return 0;
}

struct dvb_descriptors *get_dvb_ts_tables(int dmx_fd,
					  uint32_t delivery_system,
					  unsigned timeout_multiply,
					  int verbose)
{
	int i, rc;
	int other_nit, pat_pmt_time, sdt_time, nit_time;

	struct dvb_descriptors *dvb_desc;

	dvb_desc = calloc(sizeof(*dvb_desc), 1);
	if (!dvb_desc)
		return NULL;

	dvb_desc->verbose = verbose;
	dvb_desc->delivery_system = delivery_system;

	if (!timeout_multiply)
		timeout_multiply = 1;

	/* Get standard timeouts for each table */
	switch(delivery_system) {
	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
	case SYS_DVBS:
	case SYS_DVBS2:
	case SYS_TURBO:
		pat_pmt_time = 1;
		sdt_time = 2;
		nit_time = 10;
		other_nit = 1;
		break;
	case SYS_DVBT:
	case SYS_DVBT2:
		pat_pmt_time = 1;
		sdt_time = 2;
		nit_time = 12;
		other_nit = 1;
		break;
	case SYS_ISDBT:
		pat_pmt_time = 1;
		sdt_time = 2;
		nit_time = 12;
		other_nit = 0;
		break;
	case SYS_ATSC:
	case SYS_DVBC_ANNEX_B:
		pat_pmt_time = 1;
		sdt_time = 5;
		nit_time = 5;
		other_nit = 1;
	default:
		pat_pmt_time = 1;
		sdt_time = 2;
		nit_time = 10;
		other_nit = 1;
		break;
	};

	/* PAT table */
	rc = read_section(dmx_fd, dvb_desc, 0, 0, NULL,
			  pat_pmt_time * timeout_multiply);
	if (rc < 0) {
		fprintf(stderr, "error while waiting for PAT table\n");
		free_dvb_ts_tables(dvb_desc);
		return NULL;
	}

	/* PMT tables */
	for (i = 0; i < dvb_desc->pat_table.pid_table_len; i++) {
		struct pid_table *pid_table = &dvb_desc->pat_table.pid_table[i];
		uint16_t pn = pid_table->service_id;
		/* Skip PAT, CAT, reserved and NULL packets */
		if (!pn)
			continue;
		rc = read_section(dmx_fd, dvb_desc, pid_table->pid, 0x02,
			          pid_table, pat_pmt_time * timeout_multiply);
		if (rc < 0)
			fprintf(stderr, "error while reading the PMT table for service 0x%04x\n",
				pn);
	}

	/* NIT table */
	rc = read_section(dmx_fd, dvb_desc, 0x0010, 0x40, NULL,
		          nit_time * timeout_multiply);
	if (rc < 0)
		fprintf(stderr, "error while reading the NIT table\n");

	/* SDT table */
	rc = read_section(dmx_fd, dvb_desc, 0x0011, 0x42, NULL,
		          sdt_time * timeout_multiply);
	if (rc < 0)
		fprintf(stderr, "error while reading the SDT table\n");

	/* NIT/SDT other tables */
	if (other_nit) {
		if (verbose)
			printf("Parsing other NIT/SDT\n");
		rc = read_section(dmx_fd, dvb_desc, 0x0010, 0x41, NULL,
			          nit_time * timeout_multiply);
		rc = read_section(dmx_fd, dvb_desc, 0x0011, 0x46, NULL,
			          sdt_time * timeout_multiply);
	}

	return dvb_desc;
}


void free_dvb_ts_tables(struct dvb_descriptors *dvb_desc)
{
	struct pat_table *pat_table = &dvb_desc->pat_table;
	struct pid_table *pid_table = dvb_desc->pat_table.pid_table;
	struct nit_table *nit_table = &dvb_desc->nit_table;
	struct sdt_table *sdt_table = &dvb_desc->sdt_table;
	int i;

	if (pid_table) {
		for (i = 0; i < pat_table->pid_table_len; i++) {
			if (pid_table[i].video_pid)
				free(pid_table[i].video_pid);
			if (pid_table[i].audio_pid)
				free(pid_table[i].audio_pid);
			if (pid_table[i].other_el_pid)
				free(pid_table[i].other_el_pid);
		}
		free(pid_table);
	}

	if (nit_table->lcn)
		free(nit_table->lcn);
	if (nit_table->network_name)
		free(nit_table->network_name);
	if (nit_table->network_alias)
		free(nit_table->network_alias);
	if (nit_table->tr_table)
		free(nit_table->tr_table);
	if (nit_table->frequency)
		free(nit_table->frequency);
	if (nit_table->orbit)
		free(nit_table->orbit);

	if (sdt_table->service_table) {
		for (i = 0; i < sdt_table->service_table_len; i++) {
			if (sdt_table->service_table[i].provider_name)
				free(sdt_table->service_table[i].provider_name);
			if (sdt_table->service_table[i].provider_alias)
				free(sdt_table->service_table[i].provider_alias);
			if (sdt_table->service_table[i].service_name)
				free(sdt_table->service_table[i].service_name);
			if (sdt_table->service_table[i].service_alias)
				free(sdt_table->service_table[i].service_alias);
		}
		free(sdt_table->service_table);
	}
	free(dvb_desc);
}

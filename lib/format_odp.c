/* format odp support 
 *
 */

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>

#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "wandio.h"
#include "rt_protocol.h"


#define DATA(x) ((struct duck_format_data_t *)x->format_data)
#define DATAOUT(x) ((struct duck_format_data_out_t *)x->format_data)

#define OUTPUT DATAOUT(libtrace)

struct duck_format_data_t {
	char *path;
	int dag_version;
};

struct duck_format_data_out_t {
	char *path;
	int level;
	int compress_type;
	int fileflag;
	iow_t *file;
	int dag_version;	
};

static int duck_init_input(libtrace_t *libtrace) {
	libtrace->format_data = malloc(sizeof(struct duck_format_data_t));

	DATA(libtrace)->dag_version = 0;
	return 0;
}

static int duck_init_output(libtrace_out_t *libtrace) {
	libtrace->format_data = malloc(sizeof(struct duck_format_data_out_t));
	
	OUTPUT->level = 0;
	OUTPUT->compress_type = TRACE_OPTION_COMPRESSTYPE_NONE;
	OUTPUT->fileflag = O_CREAT | O_WRONLY;
	OUTPUT->file = 0;
	OUTPUT->dag_version = 0;
	return 0;
}

static int duck_config_output(libtrace_out_t *libtrace, 
				trace_option_output_t option,
				void *data) {
	switch (option) {
		case TRACE_OPTION_OUTPUT_COMPRESS:
			OUTPUT->level = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_COMPRESSTYPE:
			OUTPUT->compress_type = *(int *)data;
			return 0;
		case TRACE_OPTION_OUTPUT_FILEFLAGS:
			OUTPUT->fileflag = *(int *)data;
			return 0;
		default:
			trace_set_err_out(libtrace, TRACE_ERR_UNKNOWN_OPTION,
					"Unknown option");
			return -1;
	}
	assert(0);
}

static int duck_start_input(libtrace_t *libtrace) {
	
	if (libtrace->io)
		/* File already open */
		return 0;
	
	libtrace->io = trace_open_file(libtrace);
	if (!libtrace->io)
		return -1;

	return 0;
}

static int duck_start_output(libtrace_out_t *libtrace) {
	OUTPUT->file = trace_open_file_out(libtrace, 
						OUTPUT->compress_type,
						OUTPUT->level,
						OUTPUT->fileflag);
	if (!OUTPUT->file) {
		return -1;
	}
	return 0;
}

static int duck_fin_input(libtrace_t *libtrace) {
	wandio_destroy(libtrace->io);
	free(libtrace->format_data);

	return 0;
}

static int duck_fin_output(libtrace_out_t *libtrace) {
	wandio_wdestroy(OUTPUT->file);
	free(libtrace->format_data);
	return 0;
}

static int duck_prepare_packet(libtrace_t *libtrace, libtrace_packet_t *packet,
		void *buffer, libtrace_rt_types_t rt_type, uint32_t flags) {

        if (packet->buffer != buffer &&
                        packet->buf_control == TRACE_CTRL_PACKET) {
                free(packet->buffer);
        }

        if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER) {
                packet->buf_control = TRACE_CTRL_PACKET;
        } else
                packet->buf_control = TRACE_CTRL_EXTERNAL;


        packet->buffer = buffer;
        packet->header = NULL;
	packet->payload = buffer;
	packet->type = rt_type;

	if (libtrace->format_data == NULL) {
		if (duck_init_input(libtrace))
			return -1;
	}

	return 0;
}

static int duck_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {

	int numbytes = 0;
	uint32_t version = 0;
	unsigned int duck_size;
	uint32_t flags = 0;
	
	if (!packet->buffer || packet->buf_control == TRACE_CTRL_EXTERNAL) {
                packet->buffer = malloc((size_t)LIBTRACE_PACKET_BUFSIZE);
                if (!packet->buffer) {
                        trace_set_err(libtrace, errno,
                                        "Cannot allocate memory");
                        return -1;
                }
        }

	flags |= TRACE_PREP_OWN_BUFFER;
	
	if (DATA(libtrace)->dag_version == 0) {
		/* Read in the duck version from the start of the trace */
		if ((numbytes = wandio_read(libtrace->io, &version, 
					sizeof(version))) != sizeof(uint32_t)) {
			trace_set_err(libtrace, errno, 
					"Reading DUCK version failed");
			return -1;
		}
		if (numbytes == 0) {
			return 0;
		}
		DATA(libtrace)->dag_version = bswap_le_to_host32(version);
	}
	

	if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_2_4) {
		duck_size = sizeof(duck2_4_t);
		packet->type = TRACE_RT_DUCK_2_4;
	} else if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_2_5) {
		duck_size = sizeof(duck2_5_t);
		packet->type = TRACE_RT_DUCK_2_5;
	} else if (DATA(libtrace)->dag_version == TRACE_RT_DUCK_5_0) {
		duck_size = sizeof(duck5_0_t);
		packet->type = TRACE_RT_DUCK_5_0;
	} else {
		trace_set_err(libtrace, TRACE_ERR_BAD_PACKET,
				"Unrecognised DUCK version %i", 
				DATA(libtrace)->dag_version);
		return -1;
	}

	if ((numbytes = wandio_read(libtrace->io, packet->buffer,
					(size_t)duck_size)) != (int)duck_size) {
		if (numbytes == -1) {
			trace_set_err(libtrace, errno, "Reading DUCK failed");
			return -1;
		}
		else if (numbytes == 0) {
			return 0;
		}
		else {
			trace_set_err(libtrace, TRACE_ERR_BAD_PACKET, "Truncated DUCK packet");
		}
	}

	if (duck_prepare_packet(libtrace, packet, packet->buffer, packet->type,
				flags)) 
		return -1;
	
	return numbytes;
}

static int duck_write_packet(libtrace_out_t *libtrace, 
		libtrace_packet_t *packet) 
{

	int numbytes = 0;
	uint32_t duck_version;

	if (packet->type != TRACE_RT_DUCK_2_4 
			&& packet->type != TRACE_RT_DUCK_2_5 &&
			packet->type != TRACE_RT_DUCK_5_0) {
		trace_set_err_out(libtrace, TRACE_ERR_BAD_PACKET,
				"Only DUCK packets may be written to a DUCK file");
		return -1;
	}
	
	assert(OUTPUT->file);

	if (OUTPUT->dag_version == 0) {
	/* Writing the DUCK version will help with reading it back in later! */
		duck_version = bswap_host_to_le32(packet->type);
		if ((numbytes = wandio_wwrite(OUTPUT->file, &duck_version,
				sizeof(duck_version))) != sizeof(uint32_t)){
			trace_set_err_out(libtrace, errno, 
					"Writing DUCK version failed");
			return -1;
		}
		OUTPUT->dag_version = packet->type;
	}
	
	if ((numbytes = wandio_wwrite(OUTPUT->file, packet->payload, 
					trace_get_capture_length(packet))) !=
				(int)trace_get_capture_length(packet)) {
		trace_set_err_out(libtrace, errno, "Writing DUCK failed");
		return -1;
	}
	return numbytes;
}

static int duck_get_capture_length(const libtrace_packet_t *packet) {
	switch(packet->type) {
		case TRACE_RT_DUCK_2_4:
			return sizeof(duck2_4_t);
		case TRACE_RT_DUCK_2_5:
			return sizeof(duck2_5_t);
		case TRACE_RT_DUCK_5_0:
			return sizeof(duck5_0_t);
		default:
			trace_set_err(packet->trace,TRACE_ERR_BAD_PACKET,
					"Not a duck packet");
			return -1;
	}
	return 0;
}

static int duck_get_framing_length(const libtrace_packet_t *packet UNUSED) 
{
	return 0;
}

static int duck_get_wire_length(const libtrace_packet_t *packet UNUSED) 
{
	return 0;
}

static libtrace_linktype_t duck_get_link_type(
				const libtrace_packet_t *packet UNUSED) 
{
	return TRACE_TYPE_DUCK;
}

/* <== *** ==> */
static void odp_help(void) {
	printf("Endace ODP format module\n");
	printf("Supported input uris:\n");
	printf("\todp:/path/to/input/file\n");
	printf("Supported output uris:\n");
	printf("\todp:/path/to/output/file\n");
	printf("\n");
	return;
}

static struct libtrace_format_t odp = {
        "odp",
        "$Id$",
        TRACE_FORMAT_ODP,
	NULL,				/* probe filename */
	NULL,				/* probe magic */
        duck_init_input,	        /* init_input */
        NULL,                           /* config_input */
        duck_start_input,	        /* start_input */
        NULL,                           /* pause_input */
        duck_init_output,               /* init_output */
        duck_config_output,             /* config_output */
        duck_start_output,              /* start_output */
        duck_fin_input,	               	/* fin_input */
        duck_fin_output,                /* fin_output */
        duck_read_packet,        	/* read_packet */
        duck_prepare_packet,		/* prepare_packet */
	NULL,                           /* fin_packet */
        duck_write_packet,              /* write_packet */
        duck_get_link_type,    		/* get_link_type */
        NULL,              		/* get_direction */
        NULL,              		/* set_direction */
        NULL,          			/* get_erf_timestamp */
        NULL,                           /* get_timeval */
	NULL,				/* get_timespec */
        NULL,                           /* get_seconds */
        NULL,                   	/* seek_erf */
        NULL,                           /* seek_timeval */
        NULL,                           /* seek_seconds */
        duck_get_capture_length,  	/* get_capture_length */
        duck_get_wire_length,  		/* get_wire_length */
        duck_get_framing_length, 	/* get_framing_length */
        NULL,         			/* set_capture_length */
	NULL,				/* get_received_packets */
	NULL,				/* get_filtered_packets */
	NULL,				/* get_dropped_packets */
	NULL,				/* get_statistics */
        NULL,                           /* get_fd */
        NULL,              		/* trace_event */
        odp_help,                     	/* help */
        NULL,                            /* next pointer */
        NON_PARALLEL(false)
};

/* <== *** ==> */
void odp_constructor(void) {
	register_format(&odp);
}

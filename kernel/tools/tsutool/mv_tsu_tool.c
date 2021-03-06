#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>


#include "mv_tsu_ioctl.h"


/*
 Usage:
	TsuTool <device> <read/write> <file> <options>
*/


#define BUFSIZE		2048
#define TIMESTAMP_SIZE	4
#define TSU_TOOL_STAMP	0x4D52564C
#define FILE_HDR_SIZE	64

#define READ 		1
#define WRITE 		0

char 			*util_name;
char 			*data_buff = NULL;

int 			g_is_read = -1;
struct tsu_buff_info 	g_buff_info;
int			g_raw_mode = 0;
int			g_b2b_mode = 0;
int 			retry_on_fail = 0;
int 			g_data_blk_sz = 0;	// Actual block size <= g_buf_size
int			g_buf_size =0;		// Buffer size
unsigned int 		g_frequency = -1;
FILE 			*g_stat_fd = NULL;
int 			g_ts_data_size = 0;
int 			g_tms_data_size = 0;

void data_read(int dev_fd, FILE *file_fd);
void data_write(int dev_fd, FILE *file_fd);
int get_buff_info(int dev_fd);

void usage(void)
{
	fprintf(stderr,"Usage: %s <device> <r/w> <file> <options>\n",util_name);
	fprintf(stderr,"<file> can be set to stdout.\n");
	fprintf(stderr,"Where <options> are:\n");
	fprintf(stderr,"-r <tms file>: Write / Read data in raw TS format.\n"
	       "               <tms file> is the file that holds / will hold the timestamp information.\n");
	fprintf(stderr,"-f <freq>: The frequency at which data is to be received / transmitted.\n"
	       "           In Rx mode, this is mandatory.\n"
	       "           In Tx mode, this will override the file header frequency.\n");
	fprintf(stderr,"-b: Transmit the TS file in Back-to-Back mode, no timestamps are needed.\n");
	exit(1);
}

/*
 * Helper function for printing the TSU statistics.
 */
void tsu_print_stat(struct tsu_stat *stat)
{
	fprintf(stderr,"\tTS Interface Error Interrupts: %d.\n",stat->ts_if_err);
	fprintf(stderr,"\tTS Fifo Overflow Interrupts: %d.\n",stat->fifo_ovfl);
	fprintf(stderr,"\tTS Clock Sync Expired Interrupts: %d.\n",stat->clk_sync_exp);
	fprintf(stderr,"\tTS Connection Error Interrupts: %d.\n",stat->ts_conn_err);
	fprintf(stderr,"\n");
}

int main(int argc, char *argv[])
{
	char *dev_name;
	char *file_name;
	int dev_fd = 0;
	FILE *file_fd = NULL;
	int cnt = 0;
	struct tsu_stat tsu_stat;

	/* Eecutable name.		*/
	util_name = argv[cnt++];

	if(argc < 4) {
		if(argc > 1)
			fprintf(stderr,"Missing parameters.\n");
		usage();
	}

	/* Device name			*/
	dev_name = argv[cnt++];

	/* Read / Write operation.	*/
	if(!strcmp(argv[cnt],"r")) {
		g_is_read = 1;
	} else if(!strcmp(argv[cnt],"w")) {
		g_is_read = 0;
	} else {
		fprintf(stderr,"Bad read / write option.\n");
		usage();
	}
	cnt++;

	/* Open the device.		*/
	if(g_is_read)
		dev_fd = open(dev_name, O_RDONLY);
	else
		dev_fd = open(dev_name, O_WRONLY);

	if(dev_fd <= 0) {
		fprintf(stderr,"%s - Cannot open device %s.\n",util_name,dev_name);
		exit(2);
	}

	/* Input / Output file name.	*/
	file_name = argv[cnt++];
	if(g_is_read) {
		if(!strncmp(file_name,"stdout",strlen("stdout")))
			file_fd = stdout;
	else
			file_fd = fopen(file_name, "w+b");
	}else{
		file_fd = fopen(file_name, "rb");
	}
	if(file_fd == NULL) {
		fprintf(stderr,"%s - Cannot open file %s.\n",util_name,file_name);
		goto error;
	}

	if(get_buff_info(dev_fd) < 0){
		fprintf(stderr,"%s - Failed to retrieve device buffer configuration.\n");
		goto error;
	}

	/* Allocate buffer.		*/
	data_buff = malloc(g_buf_size);
	if(data_buff == NULL) {
		fprintf(stderr,"%s - Failed to allocate memory (%d Bytes).\n",util_name,
		       BUFSIZE);
		goto error;
	}

	if(process_options(argc,argv,cnt) < 0) {
		fprintf(stderr,"Bad options.\n");
		goto error;
	}

	/* Clear statistics.			*/
	if(ioctl(dev_fd,MVTSU_IOCCLEARSTAT,0) < 0) {
		fprintf(stderr,"Error Clearing statistics.\n");
		goto error;
	}

	if(g_is_read)
		data_read(dev_fd,file_fd);
	else
		data_write(dev_fd,file_fd);

	/* Print statistics.			*/
	if(ioctl(dev_fd,MVTSU_IOCGETSTAT,&tsu_stat) < 0) {
		fprintf(stderr,"Error Printing statistics.\n");
		goto error;
	}
	tsu_print_stat(&tsu_stat);

error:
	if(dev_fd != 0)
		close(dev_fd);
	if(file_fd != NULL)
		fclose(file_fd);
	if(data_buff != NULL)
		free(data_buff);
	if(g_stat_fd != NULL)
		fclose(g_stat_fd);
	return 0;
}


int check_write_file_device_params(struct tsu_buff_info *buff_info)
{
	if(!g_raw_mode) {
		/* buffer configuration for input data & TSU must match.*/
		if(buff_info->aggr_mode == g_buff_info.aggr_mode) {
			if((g_buff_info.aggr_mode == aggrMode1) &&
			   (g_buff_info.aggr_num_packets != 
			    buff_info->aggr_num_packets)) {
				fprintf(stderr,"Mismtach in Num of Aggregation packets (%d, %d).\n",
				       g_buff_info.aggr_num_packets,
				       buff_info->aggr_num_packets);
				return -1;
			}
			if((g_buff_info.aggr_mode == aggrMode2) &&
			   (g_buff_info.aggr_mode2_tmstmp_off != 
			    buff_info->aggr_mode2_tmstmp_off)) {
				fprintf(stderr,"Mismtach in Aggregation Timestamp offset (%d, %d).\n",
				       g_buff_info.aggr_mode2_tmstmp_off,
				       buff_info->aggr_mode2_tmstmp_off);
				return -1;
			}
		}

		if(buff_info->aggr_mode != g_buff_info.aggr_mode) {
			if(g_buff_info.aggr_mode == aggrMode1){
				fprintf(stderr,"Device configured in aggregation mode 1 while file is not.\n");
				return -1;
			}
			if(buff_info->aggr_mode == aggrMode1) {
				fprintf(stderr,"File configured in aggregation mode 1 while port is not.\n");
				return -1;
			}
			/* File @ mode2, Device @ Aggr-Disabled.	*/
			if((buff_info->aggr_mode == aggrMode2) &&
			   (buff_info->aggr_mode2_tmstmp_off != TIMESTAMP_SIZE)) {
				fprintf(stderr,"File is in aggr-mode-2 mode with timestamp "
				       "offset != 4, and device is at aggr-dis mode.\n");
				return -1;
			}

			/* Device @ mode2, File @ Aggr-Disabled.	*/
			if((g_buff_info.aggr_mode == aggrMode2) &&
			   (g_buff_info.aggr_mode2_tmstmp_off != TIMESTAMP_SIZE)) {
				fprintf(stderr,"Device is in aggr-mode-2 mode with timestamp "
				       "offset != 4, and file is at aggr-dis mode.\n");
				return -1;
			}
		}
	}
	return 0;
}


int single_read_write(int is_read, int fd, char *buf, int count)
{
	int tmp = 0;
	int ret;
        
	while(tmp < count) {

		if(is_read) {
			ret = read(fd,buf + tmp,count - tmp);
                } else {
			ret = write(fd,buf + tmp,count - tmp);
		}

		if(ret <= 0)
			return ret;
		tmp += ret;
	}
	return count;
}


int single_fread_fwrite(int is_read, FILE *fd, char *buf, int count)
{
	int tmp = 0;
	int ret;

	while(tmp < count) {

		if(is_read) {
			ret = fread(buf + tmp,1,count - tmp,fd);
                } else {
			ret = fwrite(buf + tmp,1,count - tmp,fd);
		}

		if(ret <= 0)
			return ret;
		tmp += ret;
	}
	return count;
}


void setup_buffer_timestamps(char *buff, int *s_offs, int *init_tms,int *new_size)
{
	int i;
	int total_diffs = 0;
	int first_tms = -1;
	int tmp;

	memcpy(&first_tms,buff,TIMESTAMP_SIZE);
	*init_tms = first_tms;

	if(g_buff_info.aggr_mode == aggrMode1) {
		/* Calculate the timestamp interval.		*/
		memcpy(&tmp,
		       buff + ((g_buff_info.aggr_num_packets - 1) * TIMESTAMP_SIZE),
		       TIMESTAMP_SIZE);
		total_diffs = tmp - first_tms;
		total_diffs = total_diffs / (g_buff_info.aggr_num_packets - 1);

		/* Copy timestamp interval	*/
		memcpy(buff + (g_buff_info.aggr_num_packets - 2) * TIMESTAMP_SIZE,
		       &total_diffs,TIMESTAMP_SIZE);

		/* Copy initial timestamp	*/
		memcpy(buff + (g_buff_info.aggr_num_packets - 1) * TIMESTAMP_SIZE,
		       &first_tms,TIMESTAMP_SIZE);

		*s_offs = (g_buff_info.aggr_num_packets - 2) * TIMESTAMP_SIZE;
		*new_size -= (*s_offs);
	} else {
		*s_offs = 0;
	}
	return;
}


void data_write(int dev_fd, FILE *file_fd)
{
	int cnt;
	FILE *fd;
	int tmp;
	int s_offs, e_offs;
	int pkt_size;
	int first_write = 1;
	struct tsu_buff_info buff_info;
	int init_tms;
	unsigned int freq;

	if(!g_b2b_mode) {
	/* Read TS info from file header.		*/
	/* pktsize mode aggr_pkts_num tmstmp_offset	*/
	if(!g_raw_mode)
		fd = file_fd;
	else
		fd = g_stat_fd;

	if(fread(data_buff,1,FILE_HDR_SIZE,fd) < FILE_HDR_SIZE) {
		fprintf(stderr,"Failed to read file information header.\n");
		return;
	}

	sscanf(data_buff,"%d %d %d %d %d %d",&tmp,&pkt_size,&buff_info.aggr_mode,
	       &buff_info.aggr_num_packets,&buff_info.aggr_mode2_tmstmp_off,
	       &freq);
	if(tmp != TSU_TOOL_STAMP) {
		fprintf(stderr,"Bad stamp at file header (0x%x).\n",tmp);
		return;
	}

	/* If user did not specify a frequency for TX, take the file frequency.	*/
	if(g_frequency == -1)
		g_frequency = freq;

	if(pkt_size != g_buff_info.pkt_size) {
		fprintf(stderr,"File Packet size (%d) & TSU packet size (%d) do not match.\n",
		       pkt_size,g_buff_info.pkt_size);
		return;
	}

	if(check_write_file_device_params(&buff_info) < 0)
		return;
	}

	/* Calculate the values of g_ts_data_size & g_tms_data_size.	*/
	if(g_raw_mode) {
		if(g_buff_info.aggr_mode == aggrMode1) {
			g_ts_data_size = 
				(g_buff_info.pkt_size * 
				 g_buff_info.aggr_num_packets);
			g_tms_data_size = (TIMESTAMP_SIZE * 
					   g_buff_info.aggr_num_packets);
		}
		else if(g_buff_info.aggr_mode == aggrMode2) {
			g_ts_data_size = g_buff_info.pkt_size;
			g_tms_data_size = TIMESTAMP_SIZE;
		}
		else { /* Aggregation disabled.	*/
			g_ts_data_size = g_buff_info.pkt_size;
			g_tms_data_size = TIMESTAMP_SIZE;
		}
	} else {
		g_ts_data_size = g_data_blk_sz;
		g_tms_data_size = 0;
	}

	/* Setup Tx frequency.		*/
        if(ioctl(dev_fd,MVTSU_IOCFREQSET,&g_frequency) < 0) {
		fprintf(stderr,"Error configuring port frequency.\n");
		goto done;
	}
#if 0
	printf("g_ts_data_size = %d, g_data_blk_sz = %d.\n",
	       g_ts_data_size,g_tms_data_size);
#endif 
	while(1) {
		cnt = 0;
		s_offs = 0;
		e_offs = 0;
		while(cnt < g_data_blk_sz) {
//			printf("cnt = %d, ",cnt);
			if(g_tms_data_size) {
				if(g_buff_info.aggr_mode == aggrMode2) {
					e_offs += (g_buff_info.aggr_mode2_tmstmp_off - 
						   g_tms_data_size);
					cnt += (g_buff_info.aggr_mode2_tmstmp_off - 
						   g_tms_data_size);
				}
//				printf("sread %d, ",g_tms_data_size);
				tmp = single_fread_fwrite(READ,g_stat_fd,data_buff + e_offs,
							g_tms_data_size);
				if(tmp != g_tms_data_size) {
					fprintf(stderr,"Error reading from timestamp file.\n");
					goto done;
				}
                                e_offs += tmp;
				cnt += tmp;
			}

//			printf("dread %d, ",g_ts_data_size);
			tmp = single_fread_fwrite(READ,file_fd,data_buff + e_offs,
						g_ts_data_size);
			if(tmp < 0) {
				fprintf(stderr,"Error reading from source file.\n");
				goto done;
			}
			if(tmp == 0) {
				/* No more input.	*/
				goto done;
			}
			e_offs += tmp;
			cnt += tmp;
                        setup_buffer_timestamps(data_buff,&s_offs,&init_tms,&cnt);
		}

		if(first_write) {
			struct tsu_tmstmp_info tms_info;
			tms_info.enable_tms = !g_b2b_mode;
			tms_info.timestamp = init_tms;
			if(ioctl(dev_fd,MVTSU_IOCTXTMSSET,&tms_info) < 0) {
				fprintf(stderr,"Cannot set initial timestamp for TX operation.\n");
				goto done;
			}
			first_write = 0;
		}

//		printf("twrite %d.\n",g_data_blk_sz);
		cnt = single_read_write(WRITE,dev_fd,data_buff + s_offs,
					g_data_blk_sz);
		if(cnt <= 0) {
			fprintf(stderr,"Error writing to target device.\n");
			break;
		}
	}

done:
	/* tell device that tx is over.		*/
	if(ioctl(dev_fd,MVTSU_IOCTXDONE,0) < 0) {
		fprintf(stderr,"Error in TX-Done IOCTL.\n");
		return;
	}
        return;
}

void data_read(int dev_fd, FILE *file_fd)
{
	int cnt = 0;
	int tmp;
	int offs = 0;

	/* Write TS info the file header.		*/
	/* pktsize mode aggr_pkts_num tmstmp_offset	*/
	sprintf(data_buff,"%d %d %d %d %d %d",
		TSU_TOOL_STAMP,g_buff_info.pkt_size,g_buff_info.aggr_mode,
		g_buff_info.aggr_num_packets,
		g_buff_info.aggr_mode2_tmstmp_off, g_frequency);
	tmp = strlen(data_buff);
	while(tmp < FILE_HDR_SIZE)
		data_buff[tmp++] = ' ';
	data_buff[tmp] = '\0';

	if(!g_raw_mode)
		cnt = fwrite(data_buff,1,strlen(data_buff),file_fd);
	else
		cnt = fwrite(data_buff,1,strlen(data_buff),g_stat_fd);

	if(cnt != strlen(data_buff)) {
		fprintf(stderr,"Error wrting file header.\n");
		return;
        }

	/* Calculate the values of g_ts_data_size & g_tms_data_size.	*/
	if(g_raw_mode) {
		if(g_buff_info.aggr_mode == aggrMode1) {
			g_ts_data_size = 
				(g_buff_info.pkt_size * 
				 g_buff_info.aggr_num_packets);
			g_tms_data_size = (TIMESTAMP_SIZE * 
					   g_buff_info.aggr_num_packets);
		}
		else if(g_buff_info.aggr_mode == aggrMode2) {
			g_ts_data_size = g_buff_info.pkt_size;
			g_tms_data_size = g_buff_info.aggr_mode2_tmstmp_off;
		}
		else { /* Aggregation disabled.	*/
			g_ts_data_size = g_buff_info.pkt_size;
			g_tms_data_size = TIMESTAMP_SIZE;
		}
	} else {
		g_ts_data_size = g_data_blk_sz;
		g_tms_data_size = 0;
	}

	/* Setup frequency.		*/
        if(ioctl(dev_fd,MVTSU_IOCFREQSET,&g_frequency) < 0) {
		fprintf(stderr,"Error configuring port frequency.\n");
		goto done;
	}

//	fprintf(stderr,"g_ts_data_size = %d, g_tms_data_size = %d.\n",
//	       g_ts_data_size,g_tms_data_size);
	cnt = 0;
	while(1) {

		if(offs != 0)
			fprintf(stderr,"offs = %d.\n");
		tmp = single_read_write(READ,dev_fd,data_buff + offs,
					g_data_blk_sz - offs);
		if(tmp < 0) {
			fprintf(stderr,"Error reading from source device / file.\n");
			break;
		}
		cnt += tmp;
		if(cnt == 0)
			break;

		while(cnt >= (g_ts_data_size + g_tms_data_size)) {
 //                       fprintf(stderr,"cnt - %d, ",cnt);
			{
				int ii;
//				for(ii = 0; ii < 8; ii++)
//					fprintf(stderr,"%02x ",(data_buff + offs)[ii]);
//				fprintf(stderr,"\n");
			}
			tmp = 0;
			if(g_tms_data_size > 0) {
				if(g_buff_info.aggr_mode == aggrMode2) {
//					fprintf(stderr,"TMSW = %d, ",offs);
					/* write only the timestamp part.	*/
					tmp = single_fread_fwrite(WRITE,g_stat_fd,
								  data_buff + offs,
						    TIMESTAMP_SIZE);
					if(tmp < TIMESTAMP_SIZE) {
						fprintf(stderr,"Error writing to timestamps file.\n");
						goto done;
					}
				} else {
					tmp = single_fread_fwrite(WRITE,g_stat_fd,
								data_buff + offs,
								g_tms_data_size);
					if(tmp < g_tms_data_size) {
						fprintf(stderr,"Error writing to timestamps file.\n");
						goto done;
					}
				}

				offs += tmp;
			}
//			fprintf(stderr,"TSDW = %d.\n",offs);
			tmp = single_fread_fwrite(WRITE,file_fd,data_buff + offs,
						g_ts_data_size);
			if(tmp < g_ts_data_size) {
				fprintf(stderr,"Error writing to data file.\n");
				goto done;
			}
			offs += g_ts_data_size;
			cnt -= (g_ts_data_size + g_tms_data_size);
		}

		if(cnt > 0) {
			memmove(data_buff,data_buff + offs, cnt);
			offs = cnt;
		}
		else {
			offs = 0;
		}
	}
done:
	return;
}


int get_buff_info(int dev_fd)
{
	char *tmp;

	if(ioctl(dev_fd,MVTSU_IOCBUFFPARAMGET,&g_buff_info) < 0){
		fprintf(stderr,"Error reading device buffer information.\n");
		return -1;
	}

	if(g_buff_info.aggr_mode == aggrMode1) {
		if(g_is_read)
			g_data_blk_sz = ((g_buff_info.pkt_size + TIMESTAMP_SIZE) *
					 g_buff_info.aggr_num_packets);
		else
			g_data_blk_sz = ((g_buff_info.pkt_size *
					  g_buff_info.aggr_num_packets) + 
					 (2 * TIMESTAMP_SIZE));
		g_buf_size = ((g_buff_info.pkt_size + TIMESTAMP_SIZE) *
			      g_buff_info.aggr_num_packets);
		tmp = "Mode 1";
	} else if(g_buff_info.aggr_mode == aggrMode2) {
		g_data_blk_sz = ((g_buff_info.pkt_size + 
				  g_buff_info.aggr_mode2_tmstmp_off) *
				 g_buff_info.aggr_num_packets);
		g_buf_size = g_data_blk_sz;
		tmp = "Mode 2";
	} else {
		g_data_blk_sz = (g_buff_info.pkt_size + TIMESTAMP_SIZE);
		g_buf_size = g_data_blk_sz;
		tmp = "Disabled";
	}
#if 0
	fprintf(stderr,"\n");
        fprintf(stderr,"\tPacket Size: %d Bytes\n",g_buff_info.pkt_size);
	fprintf(stderr,"\tAggregation %s\n",tmp);
	if(g_buff_info.aggr_mode != aggrModeDisabled)
		fprintf(stderr,"\tAggregation packets: %d.\n",g_buff_info.aggr_num_packets);
	if(g_buff_info.aggr_mode == aggrMode2)
		fprintf(stderr,"\tAggregation Timestamp offset: %d.\n",
		       g_buff_info.aggr_mode2_tmstmp_off);
#endif 
//	fprintf(stderr,"\tCalculated data buffer size: %d Bytes.\n",g_buf_size);
//	fprintf(stderr,"\tCalculated data-block size: %d Bytes.\n",g_data_blk_sz);
	return 0;
}


int process_options(int argc, char *argv[],int idx)
{
	char *stat_file = NULL;

	while(idx < argc) {
		if(argv[idx][0] != '-')
			goto error;
		switch (argv[idx][1]) {
		case 'r':
			g_raw_mode = 1;
			stat_file = argv[idx+1];
			idx+=2;
			break;
		case 'f':
			g_frequency = atoi(argv[idx+1]);
			idx+=2;
			break;
		case 'b':
			g_raw_mode = 1;
			g_b2b_mode = 1;
			stat_file = "/dev/zero";
			idx += 1;
			break;
		default:
			goto error;
		}
	}

	if(g_b2b_mode && g_is_read) {
		fprintf(stderr,"%s - Cannot work in Back-To-Back mode in read direction.",
		       util_name);
		goto error;
	}

	if(g_raw_mode) {
		if(g_is_read)
                        g_stat_fd = fopen(stat_file, "w+b");
		else
                        g_stat_fd = fopen(stat_file, "rb");
		if(g_stat_fd == NULL){
			fprintf(stderr,"%s - Cannot open file %s.\n",util_name,stat_file);
			goto error;
		}
	}

	if(g_is_read) {
		if(g_frequency == -1) {
			fprintf(stderr,"%s - Must provide data Rx frequency.\n",util_name);
			goto error;
		}
	}
	return 0;

error:
	return -1;
}

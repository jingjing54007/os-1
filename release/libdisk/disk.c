/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <phk@login.dknet.dk> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.   Poul-Henning Kamp
 * ----------------------------------------------------------------------------
 *
 * $Id: disk.c,v 1.6 1995/04/30 06:09:26 phk Exp $
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/diskslice.h>
#include "libdisk.h"

#define DOSPTYP_EXTENDED        5
#define DOSPTYP_ONTRACK         84      

struct disk *
Open_Disk(char *name)
{
	return Int_Open_Disk(name,0);
}

struct disk *
Int_Open_Disk(char *name, u_long size)
{
	int i,fd;
	struct diskslices ds;
	struct disklabel dl;
	char device[64];
	struct disk *d;
	struct dos_partition *dp;
	void *p;

	strcpy(device,"/dev/r");
	strcat(device,name);

	d = (struct disk *)malloc(sizeof *d);
	if(!d) err(1,"malloc failed");
	memset(d,0,sizeof *d);

	fd = open(device,O_RDONLY);
	if (fd < 0) {
		warn("open(%s) failed",device);
		return 0;
	}

	memset(&dl,0,sizeof dl);
	ioctl(fd,DIOCGDINFO,&dl);
	i = ioctl(fd,DIOCGSLICEINFO,&ds);
	if (i < 0) {
		warn("DIOCGSLICEINFO(%s) failed",device);
		close(fd);
		return 0;
	}

	if (!size)
		size = ds.dss_slices[WHOLE_DISK_SLICE].ds_size;

	p = read_block(fd,0);
	dp = (struct dos_partition*)(p+DOSPARTOFF);
	for(i=0;i<NDOSPART;i++) {
		if (dp->dp_start >= size) continue;
		if (dp->dp_start+dp->dp_size >= size) continue;
		if (!dp->dp_size) continue;

		if (dp->dp_typ == DOSPTYP_ONTRACK)
			d->flags |= DISK_ON_TRACK;
			
	}
	free(p);

	d->bios_sect = dl.d_nsectors;
	d->bios_hd = dl.d_ntracks;

	d->name = strdup(name);


	if (dl.d_ntracks && dl.d_nsectors)
		d->bios_cyl = size/(dl.d_ntracks*dl.d_nsectors);

	if (Add_Chunk(d, 0, size, name,whole,0,0))
		warn("Failed to add 'whole' chunk");

	if (ds.dss_slices[COMPATIBILITY_SLICE].ds_offset)
		if (Add_Chunk(d, 0, 1, "-",reserved,0,0))
			warn("Failed to add MBR chunk");
	
	for(i=BASE_SLICE;i < 12 &&  i<ds.dss_nslices;i++) {
		char sname[20];
		chunk_e ce;
		u_long flags=0;
		int subtype=0;
		if (! ds.dss_slices[i].ds_size)
			continue;
		sprintf(sname,"%ss%d",name,i-1);
		switch (ds.dss_slices[i].ds_type) {
			case 0xa5:
				ce = freebsd;
				break;
			case 0x1:
			case 0x6:
				ce = fat;
				break;
			case DOSPTYP_EXTENDED:
				ce = extended;
				break;
			default:
				ce = foo;
				subtype = -ds.dss_slices[i].ds_type;
				break;
		}	
		flags |= CHUNK_ALIGN;
		if (Add_Chunk(d,ds.dss_slices[i].ds_offset,
			ds.dss_slices[i].ds_size, sname,ce,subtype,flags))
			warn("failed to add chunk for slice %d",i - 1);
		if (ce == extended)
			if (Add_Chunk(d,ds.dss_slices[i].ds_offset,
				1, "-",reserved, subtype, flags))
				warn("failed to add MBR chunk for slice %d",i - 1);
		if (ds.dss_slices[i].ds_type == 0xa5) {
			struct disklabel *dl;

			dl = read_disklabel(fd,
				ds.dss_slices[i].ds_offset + LABELSECTOR);
			if(dl) {
				char pname[20];
				int j;
				u_long l;
				if (dl->d_partitions[RAW_PART].p_offset == 0 &&
				    dl->d_partitions[RAW_PART].p_size ==
					ds.dss_slices[i].ds_size)
					l = ds.dss_slices[i].ds_offset;
				else
					l = 0;
				for(j=0; j < dl->d_npartitions; j++) {
					sprintf(pname,"%s%c",sname,j+'a');
					if (j == RAW_PART || j == 3)
						continue;
					if (!dl->d_partitions[j].p_size)
						continue;
					if (Add_Chunk(d,
						dl->d_partitions[j].p_offset +
						l,
						dl->d_partitions[j].p_size,
						pname,part,0,0))
						warn(
	"Failed to add chunk for partition %c [%lu,%lu]",
		j + 'a',dl->d_partitions[j].p_offset,dl->d_partitions[j].p_size);
				}
				sprintf(pname,"%sd",sname);
				if (dl->d_partitions[3].p_size)
					Add_Chunk(d,
						dl->d_partitions[3].p_offset +
						l,
						dl->d_partitions[3].p_size,
						pname,part,0,0);
				free(dl);
			}
		}
	}
	close(fd);
	return d;
}

void
Debug_Disk(struct disk *d)
{
	printf("Debug_Disk(%s)",d->name);
	printf("  flags=%lx",d->flags);
	printf("  real_geom=%lu/%lu/%lu",d->real_cyl,d->real_hd,d->real_sect);
	printf("  bios_geom=%lu/%lu/%lu\n",d->bios_cyl,d->bios_hd,d->bios_sect);
	printf("  boot1=%p, boot2=%p, bootmgr=%p\n",
		d->boot1,d->boot2,d->bootmgr);
	Debug_Chunk(d->chunks);
}

void
Free_Disk(struct disk *d)
{
	if(d->chunks) Free_Chunk(d->chunks);
	if(d->name) free(d->name);
	if(d->bootmgr) free(d->bootmgr);
	if(d->boot1) free(d->boot1);
	if(d->boot2) free(d->boot2);
	free(d);
}

struct disk *
Clone_Disk(struct disk *d)
{
	struct disk *d2;

	d2 = (struct disk*) malloc(sizeof *d2);
	if(!d2) err(1,"malloc failed");
	*d2 = *d;
	d2->name = strdup(d2->name);
	d2->chunks = Clone_Chunk(d2->chunks);
	if(d2->bootmgr) {
		d2->bootmgr = malloc(DOSPARTOFF);
		memcpy(d2->bootmgr,d->bootmgr,DOSPARTOFF);
	}
	if(d2->boot1) {
		d2->boot1 = malloc(512);
		memcpy(d2->boot1,d->boot1,512);
	}
	if(d2->boot2) {
		d2->boot2 = malloc(512*7);
		memcpy(d2->boot2,d->boot2,512*7);
	}
	return d2;
}

void
Collapse_Disk(struct disk *d)
{

	while(Collapse_Chunk(d,d->chunks))
		;
}

static char * device_list[] = {"wd","sd",0};

char **
Disk_Names()
{
    int i,j,k;
    char disk[25];
    char diskname[25];
    struct stat st;
    struct diskslices ds;
    int fd;
    static char **disks;

    disks = malloc(sizeof *disks * (1 + MAX_NO_DISKS));
    memset(disks,0,sizeof *disks * (1 + MAX_NO_DISKS));
    k = 0;	
	for (j = 0; device_list[j]; j++) {
		for (i = 0; i < 10; i++) {
			sprintf(diskname, "%s%d", device_list[j], i);
			sprintf(disk, "/dev/r%s", diskname);
			if (stat(disk, &st) || !(st.st_mode & S_IFCHR))
				continue;
			if ((fd = open(disk, O_RDWR)) == -1)
				continue;
			if (ioctl(fd, DIOCGSLICEINFO, &ds) == -1) {
				close(fd);
				continue;
			}
			disks[k++] = strdup(diskname);
			if(k == MAX_NO_DISKS)
				return disks;
		}
	}
	return disks;
}

void
Set_Boot_Mgr(struct disk *d, u_char *b)
{
	if (d->bootmgr)
		free(d->bootmgr);	
	d->bootmgr = malloc(DOSPARTOFF);
	if(!d->bootmgr) err(1,"malloc failed");
	memcpy(d->bootmgr,b,DOSPARTOFF);
}

void
Set_Boot_Blocks(struct disk *d, u_char *b1, u_char *b2)
{
	if (d->boot1) free(d->boot1);	
	d->boot1 = malloc(512);
	if(!d->boot1) err(1,"malloc failed");
	memcpy(d->boot1,b1,512);
	if (d->boot2) free(d->boot2);	
	d->boot2 = malloc(7*512);
	if(!d->boot2) err(1,"malloc failed");
	memcpy(d->boot2,b2,7*512);
}

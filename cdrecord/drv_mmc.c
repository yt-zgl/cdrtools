/* @(#)drv_mmc.c	1.24 98/04/15 Copyright 1997 J. Schilling */
#ifndef lint
static	char sccsid[] =
	"@(#)drv_mmc.c	1.24 98/04/15 Copyright 1997 J. Schilling";
#endif
/*
 *	CDR device implementation for
 *	SCSI-3/mmc conforming drives
 *	e.g. Yamaha CDR-400, Ricoh MP6200
 *
 *	Currently using close_track_philips() XXX FIXME
 *
 *	Copyright (c) 1997 J. Schilling
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*#define	DEBUG*/
#define	PRINT_ATIP
#include <mconfig.h>

#include <stdio.h>
#include <standard.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/file.h>

#include <utypes.h>
#include <btorder.h>
#include <scgio.h>
#include <scsidefs.h>
#include <scsireg.h>

#include "cdrecord.h"
#include "scsitransp.h"

extern	BOOL	isgui;

struct	scg_cmd		scmd;
struct	scsi_inquiry	inq;

extern	int	scsibus;
extern	int	target;
extern	int	lun;

extern	int	silent;
extern	int	verbose;
extern	int	lverbose;

typedef struct opc {
	u_char	opc_speed[2];
	u_char	opc_val[6];
} opc_t;

#if defined(_BIT_FIELDS_LTOH)	/* Intel byteorder */

struct disk_info {
	u_char	data_len[2];		/* Data len without this info	*/
	Ucbit	disk_status	: 2;	/* Status of the disk		*/
	Ucbit	sess_status	: 2;	/* Status of last session	*/
	Ucbit	erasable	: 1;	/* Disk is erasable		*/
	Ucbit	res2		: 3;	/* Reserved			*/
	u_char	first_track;		/* # of first track on disk	*/
	u_char	numsess;		/* # of sessions		*/
	u_char	first_track_ls;		/* First track in last sessaion	*/
	u_char	last_track_ls;		/* Last track in last sessaion	*/
	Ucbit	res7_04		: 5;	/* Reserved			*/
	Ucbit	uru		: 1;	/* This is an unrestricted disk	*/
	Ucbit	dbc_v		: 1;	/* Disk bar code valid		*/
	Ucbit	did_v		: 1;	/* Disk id valid		*/
	u_char	disk_type;		/* Disk type			*/
	u_char	res9[3];		/* Reserved			*/
	u_char	disk_id[4];		/* Disk identification		*/
	u_char	last_lead_in[4];	/* Last session lead in time	*/
	u_char	last_lead_out[4];	/* Last session lead out time	*/
	u_char	disk_barcode[8];	/* Disk bar code		*/
	u_char	res32;			/* Reserved			*/
	u_char	num_opc_entries;	/* # of OPC table entries	*/
	opc_t	opc_table[1];		/* OPC table 			*/
};

#else				/* Motorola byteorder */

struct disk_info {
	u_char	data_len[2];		/* Data len without this info	*/
	Ucbit	res2		: 3;	/* Reserved			*/
	Ucbit	erasable	: 1;	/* Disk is erasable		*/
	Ucbit	sess_status	: 2;	/* Status of last session	*/
	Ucbit	disk_status	: 2;	/* Status of the disk		*/
	u_char	first_track;		/* # of first track on disk	*/
	u_char	numsess;		/* # of sessions		*/
	u_char	first_track_ls;		/* First track in last sessaion	*/
	u_char	last_track_ls;		/* Last track in last sessaion	*/
	Ucbit	did_v		: 1;	/* Disk id valid		*/
	Ucbit	dbc_v		: 1;	/* Disk bar code valid		*/
	Ucbit	uru		: 1;	/* This is an unrestricted disk	*/
	Ucbit	res7_04		: 5;	/* Reserved			*/
	u_char	disk_type;		/* Disk type			*/
	u_char	res9[3];		/* Reserved			*/
	u_char	disk_id[4];		/* Disk identification		*/
	u_char	last_lead_in[4];	/* Last session lead in time	*/
	u_char	last_lead_out[4];	/* Last session lead out time	*/
	u_char	disk_barcode[8];	/* Disk bar code		*/
	u_char	res32;			/* Reserved			*/
	u_char	num_opc_entries;	/* # of OPC table entries	*/
	opc_t	opc_table[1];		/* OPC table 			*/
};

#endif

LOCAL	int	mmc_load		__PR((void));
LOCAL	int	mmc_unload		__PR((void));
LOCAL	cdr_t	*identify_mmc		__PR((cdr_t *, struct scsi_inquiry *));
LOCAL	int	get_diskinfo		__PR((struct disk_info *dip));
LOCAL	void	di_to_dstat		__PR((struct disk_info *dip, dstat_t *dsp));
#ifdef	PRINT_ATIP
LOCAL	int	get_pma			__PR((void));
#endif
LOCAL	int	getdisktype_mmc		__PR((cdr_t *dp, dstat_t *dsp));
LOCAL	int	speed_select_mmc	__PR((int speed, int dummy));
LOCAL	int	next_wr_addr_mmc	__PR((int track, track_t *trackp, long *ap));
LOCAL	int	open_track_mmc		__PR((cdr_t *dp, int track, track_t *trackp));
LOCAL	int	close_track_mmc		__PR((int track, track_t *trackp));
LOCAL	int	open_session_mmc	__PR((int tracks, track_t *trackp, int toctype, int multi));
LOCAL	int	fixate_mmc		__PR((int onp, int dummy, int toctype, int tracks, track_t *trackp));
LOCAL	int	blank_mmc		__PR((long addr, int blanktype));

LOCAL int
mmc_load()
{
	return (scsi_load_unload(1));
}

LOCAL int
mmc_unload()
{
	return (scsi_load_unload(0));
}

cdr_t	cdr_mmc = {
	0,
	CDR_TAO|CDR_DAO|CDR_PACKET|CDR_SWABAUDIO,
	"mmc_cdr",
	"generic SCSI-3/mmc CD-R driver",
	0,
	identify_mmc,
	drive_attach,
	getdisktype_mmc,
	scsi_load,
/*	mmc_load,*/
	scsi_unload,
	recovery_needed,
	recover,
	speed_select_mmc,
	select_secsize,
	next_wr_addr_mmc,
	reserve_track,
	scsi_cdr_write,
	open_track_mmc,
	close_track_mmc,
	open_session_mmc,
	cmd_dummy,
	read_session_offset,
	fixate_mmc,
	blank_mmc,
};

/*
 * SCSI-3/mmc conformant CD drive
 */
cdr_t	cdr_cd = {
	0,
	CDR_ISREADER|CDR_SWABAUDIO,
	"mmc_cd",
	"generic SCSI-3/mmc CD driver",
	0,
	identify_mmc,
	drive_attach,
	drive_getdisktype,
	scsi_load,
	scsi_unload,
	cmd_dummy, 				/* recovery_needed	*/
	(int(*)__PR((int)))cmd_dummy,		/* recover		*/
	speed_select_mmc,
	select_secsize,
	(int(*)__PR((int, track_t *, long *)))cmd_dummy,	/* next_wr_addr		*/
	reserve_track,
	scsi_cdr_write,
	open_track_mmc,
	close_track_mmc,
	(int(*)__PR((int, track_t *, int, int)))cmd_dummy,
	cmd_dummy,
	read_session_offset,
	fixation,
	blank_dummy,
};

/*
 * Old pre SCSI-3/mmc CD drive
 */
cdr_t	cdr_oldcd = {
	0,
	CDR_ISREADER,
	"scsi2_cd",
	"generic SCSI-2 CD driver",
	0,
	identify_mmc,
	drive_attach,
	drive_getdisktype,
	scsi_load,
	scsi_unload,
	cmd_dummy, 				/* recovery_needed	*/
	(int(*)__PR((int)))cmd_dummy,		/* recover		*/
	speed_select_mmc,
	select_secsize,
	(int(*)__PR((int, track_t *, long *)))cmd_dummy,	/* next_wr_addr		*/
	reserve_track,
	scsi_cdr_write,
	open_track_mmc,
	close_track_mmc,
	(int(*)__PR((int, track_t *, int, int)))cmd_dummy,
	cmd_dummy,
	read_session_offset_philips,
	fixation,
	blank_dummy,
};

struct cd__mode_data {
	struct scsi_mode_header	header;
	union cd_pagex	{
		struct cd_mode_page_05	page05;
		struct cd_mode_page_2A	page2A;
	} pagex;
};

LOCAL cdr_t *
identify_mmc(dp, ip)
	cdr_t			*dp;
	struct scsi_inquiry	*ip;
{
	BOOL	cdrr	 = FALSE;	/* Read CD-R	*/
	BOOL	cdwr	 = FALSE;	/* Write CD-R	*/
	BOOL	cdrrw	 = FALSE;	/* Read CD-RW	*/
	BOOL	cdwrw	 = FALSE;	/* Write CD-RW	*/
	struct	cd_mode_page_2A *mp;

	if (ip->type != INQ_WORM && ip->type != INQ_ROMD)
		return ((cdr_t *)0);

	allow_atapi(TRUE);	/* Try to switch to 10 byte mode cmds */

	silent++;
	mp = mmc_cap(NULL);	/* Get MMC capabilities in allocated mp */
	silent--;
	if (mp == NULL)
		return (&cdr_oldcd);	/* Pre SCSI-3/mmc drive	 	*/

	mmc_getval(mp, &cdrr, &cdwr, &cdrrw, &cdwrw, NULL);

	/*
	 * At this point we know that we have a SCSI-3/mmc compliant drive.
	 * Unfortunately ATAPI drives violate the SCSI spec in returning
	 * a response data format of '1' which from the SCSI spec would
	 * tell us not to use the "PF" bit in mode select. As ATAPI drives
	 * require the "PF" bit to be set, we 'correct' the inquiry data.
	 */
	if (ip->data_format < 2)
		ip->data_format = 2;

	if (!cdwr)			/* SCSI-3/mmc CD drive		*/
		dp = &cdr_cd;

	dp->cdr_cdcap = mp;		/* Store MMC cap pointer	*/

	if (mp->loading_type == LT_TRAY)
		dp->cdr_flags |= CDR_TRAYLOAD;
	else if (mp->loading_type == LT_CADDY)
		dp->cdr_flags |= CDR_CADDYLOAD;

	return (dp);
}

#ifdef	PRINT_ATIP

struct tocheader {
	char	len[2];
	char	first;
	char	last;
};

#if defined(_BIT_FIELDS_LTOH)	/* Intel byteorder */

struct atipdesc {
	Ucbit	ref_speed	: 3;	/* Reference speed		*/
	Ucbit	res4_3		: 1;	/* Reserved			*/
	Ucbit	ind_wr_power	: 3;	/* Indicative tgt writing power	*/
	Ucbit	res4_7		: 1;	/* Reserved (must be "1")	*/
	Ucbit	res5_05		: 6;	/* Reserved			*/
	Ucbit	uru		: 1;	/* Disk is for unrestricted use	*/
	Ucbit	res5_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	a3_v		: 1;	/* A 3 Values valid		*/
	Ucbit	a2_v		: 1;	/* A 2 Values valid		*/
	Ucbit	a1_v		: 1;	/* A 1 Values valid		*/
	Ucbit	sub_type	: 3;	/* Disc sub type		*/
	Ucbit	erasable	: 1;	/* Disk is erasable		*/
	Ucbit	res6_7		: 1;	/* Reserved (must be "1")	*/
	u_char	lead_in[4];		/* Lead in time			*/
	u_char	lead_out[4];		/* Lead out time		*/
	u_char	res15;			/* Reserved			*/
	Ucbit	clv_high	: 4;	/* Highes usable CLV recording speed */
	Ucbit	clv_low		: 3;	/* Lowest usable CLV recording speed */
	Ucbit	res16_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	res17_0		: 1;	/* Reserved			*/
	Ucbit	tgt_y_pow	: 3;	/* Tgt y val of the power mod fun */
	Ucbit	power_mult	: 3;	/* Power multiplication factor	*/
	Ucbit	res17_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	res_18_30	: 4;	/* Reserved			*/
	Ucbit	rerase_pwr_ratio: 3;	/* Recommended erase/write power*/
	Ucbit	res18_7		: 1;	/* Reserved (must be "1")	*/
	u_char	res19;			/* Reserved			*/
	u_char	a2[3];			/* A 2 Values			*/
	u_char	res23;			/* Reserved			*/
	u_char	a3[3];			/* A 3 Vaules			*/
	u_char	res27;			/* Reserved			*/
};

#else				/* Motorola byteorder */

struct atipdesc {
	Ucbit	res4_7		: 1;	/* Reserved (must be "1")	*/
	Ucbit	ind_wr_power	: 3;	/* Indicative tgt writing power	*/
	Ucbit	res4_3		: 1;	/* Reserved			*/
	Ucbit	ref_speed	: 3;	/* Reference speed		*/
	Ucbit	res5_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	uru		: 1;	/* Disk is for unrestricted use	*/
	Ucbit	res5_05		: 6;	/* Reserved			*/
	Ucbit	res6_7		: 1;	/* Reserved (must be "1")	*/
	Ucbit	erasable	: 1;	/* Disk is erasable		*/
	Ucbit	sub_type	: 3;	/* Disc sub type		*/
	Ucbit	a1_v		: 1;	/* A 1 Values valid		*/
	Ucbit	a2_v		: 1;	/* A 2 Values valid		*/
	Ucbit	a3_v		: 1;	/* A 3 Values valid		*/
	u_char	lead_in[4];		/* Lead in time			*/
	u_char	lead_out[4];		/* Lead out time		*/
	u_char	res15;			/* Reserved			*/
	Ucbit	res16_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	clv_low		: 3;	/* Lowest usable CLV recording speed */
	Ucbit	clv_high	: 4;	/* Highes usable CLV recording speed */
	Ucbit	res17_7		: 1;	/* Reserved (must be "0")	*/
	Ucbit	power_mult	: 3;	/* Power multiplication factor	*/
	Ucbit	tgt_y_pow	: 3;	/* Tgt y val of the power mod fun */
	Ucbit	res17_0		: 1;	/* Reserved			*/
	Ucbit	res18_7		: 1;	/* Reserved (must be "1")	*/
	Ucbit	rerase_pwr_ratio: 3;	/* Recommended erase/write power*/
	Ucbit	res_18_30	: 4;	/* Reserved			*/
	u_char	res19;			/* Reserved			*/
	u_char	a2[3];			/* A 2 Values			*/
	u_char	res23;			/* Reserved			*/
	u_char	a3[3];			/* A 3 Vaules			*/
	u_char	res27;			/* Reserved			*/
};

#endif

struct atipinfo {
	struct tocheader	hd;
	struct atipdesc		desc;
};

LOCAL	int	get_atip		__PR((struct atipinfo *atp));
	void	print_di		__PR((struct disk_info *dip));
	void	print_atip		__PR((struct atipinfo *atp));
#endif	/* PRINT_ATIP */

LOCAL int
get_diskinfo(dip)
	struct disk_info *dip;
{
	int	len;
	int	ret;

	fillbytes((caddr_t)dip, sizeof(*dip), '\0');

	if (read_disk_info((caddr_t)dip, 2) < 0)
		return (-1);
	len = a_to_u_short(dip->data_len);
	len += 2;
	ret = read_disk_info((caddr_t)dip, len);

#ifdef	DEBUG
	scsiprbytes("Disk info:", (u_char *)dip,
				len-scsigetresid());
#endif
	return (ret);
}

LOCAL void
di_to_dstat(dip, dsp)
	struct disk_info	*dip;
	dstat_t	*dsp;
{
	dsp->ds_diskid = a_to_u_long(dip->disk_id);
	if (dip->did_v)
		dsp->ds_flags |= DSF_DID_V;
	dsp->ds_diskstat = dip->disk_status;
	dsp->ds_sessstat = dip->sess_status;

	dsp->ds_maxblocks = msf_to_lba(dip->last_lead_out[1],
					dip->last_lead_out[2],
					dip->last_lead_out[3]);
	/*
	 * Check for 0xFF:0xFF/0xFF which is an indicator for a complete disk
	 */
	if (dsp->ds_maxblocks == 716730)
		dsp->ds_maxblocks = -1L;
}

#ifdef	PRINT_ATIP

LOCAL int
get_atip(atp)
	struct atipinfo *atp;
{
	int	len;
	int	ret;

	fillbytes((caddr_t)atp, sizeof(*atp), '\0');

	if (read_toc((caddr_t)atp, 0, 2, 0, FMT_ATIP) < 0)
		return (-1);
	len = a_to_u_short(atp->hd.len);
	len += 2;
	ret = read_toc((caddr_t)atp, 0, len, 0, FMT_ATIP);

#ifdef	DEBUG
	scsiprbytes("ATIP info:", (u_char *)atp,
				len-scsigetresid());
#endif
	/*
	 * Yamaha sometimes returns zeroed ATIP info for disks without ATIP
	 */
	if (atp->desc.lead_in[1] == 0 &&
			atp->desc.lead_in[2] == 0 &&
			atp->desc.lead_in[3] == 0 &&
			atp->desc.lead_out[1] == 0 &&
			atp->desc.lead_out[2] == 0 &&
			atp->desc.lead_out[3] == 0)
		return (-1);

	return (ret);
}

LOCAL int
/*get_pma(atp)*/
get_pma()
/*	struct atipinfo *atp;*/
{
	int	len;
	int	ret;
char	atp[1024];

	fillbytes((caddr_t)atp, sizeof(*atp), '\0');

/*	if (read_toc((caddr_t)atp, 0, 2, 1, FMT_PMA) < 0)*/
	if (read_toc((caddr_t)atp, 0, 2, 0, FMT_PMA) < 0)
		return (-1);
/*	len = a_to_u_short(atp->hd.len);*/
	len = a_to_u_short(atp);
	len += 2;
/*	ret = read_toc((caddr_t)atp, 0, len, 1, FMT_PMA);*/
	ret = read_toc((caddr_t)atp, 0, len, 0, FMT_PMA);

#ifdef	DEBUG
	scsiprbytes("PMA:", (u_char *)atp,
				len-scsigetresid());
#endif
	ret = read_toc((caddr_t)atp, 0, len, 1, FMT_PMA);

#ifdef	DEBUG
	scsiprbytes("PMA:", (u_char *)atp,
				len-scsigetresid());
#endif
	return (ret);
}

#endif	/* PRINT_ATIP */

LOCAL int
getdisktype_mmc(dp, dsp)
	cdr_t	*dp;
	dstat_t	*dsp;
{
extern	char	*buf;
	struct disk_info *dip;
	u_char	mode[0x100];
	char	ans[2];
	msf_t	msf;

	msf.msf_min = msf.msf_sec = msf.msf_frame = 0;
#ifdef	PRINT_ATIP
	/*
	 * It seems that there are drives that do not support to
	 * read ATIP (e.g. HP 7100)
	 * Also if a NON CD-R media is inserted, this will never work.
	 * For this reason, make a failure non-fatal.
	 */
	silent++;
	if (get_atip((struct atipinfo *)mode) >= 0) {
		msf.msf_min =		mode[8];
		msf.msf_sec =		mode[9];
		msf.msf_frame =		mode[10];
		if (lverbose) {
			print_atip((struct atipinfo *)mode);
			pr_manufacturer(&msf);
		}
	}
	silent--;
#endif
	dip = (struct disk_info *)buf;
	if (get_diskinfo(dip) < 0)
		return (-1);

	/*
	 * Check for non writable disk first.
	 */
	if (dip->disk_status == DS_COMPLETE) {
		errmsgno(EX_BAD, "Drive needs to reload the media to return to proper status.\n");
		unload_media(dp, F_EJECT);

		if ((dp->cdr_flags & CDR_TRAYLOAD) == 0) {
			printf("Re-load disk and hit <CR>");
			if (isgui)
				printf("\n");
			flush();
			getline(ans, 1);
		}
		load_media(dp);
	}
	if (get_diskinfo(dip) < 0)
		return (-1);
	di_to_dstat(dip, dsp);


#ifdef	PRINT_ATIP
#ifdef	DEBUG
	if (get_atip((struct atipinfo *)mode) < 0)
		return (-1);
	/*
	 * Get pma gibt �rger mit CW-7502
	 * Wenn Die Disk leer ist, dann stuerzt alles ab.
	 * Firmware 4.02 kann nicht get_pma
	 */
	if (dip->disk_status != DS_EMPTY) {
/*		get_pma();*/
	}
	printf("ATIP lead in:  %ld (%02d:%02d/%02d)\n",
		msf_to_lba(mode[8], mode[9], mode[10]),
		mode[8], mode[9], mode[10]);
	printf("ATIP lead out: %ld (%02d:%02d/%02d)\n",
		msf_to_lba(mode[12], mode[13], mode[14]),
		mode[12], mode[13], mode[14]);
	print_di(dip);
	print_atip((struct atipinfo *)mode);
#endif
#endif	/* PRINT_ATIP */
	return (drive_getdisktype(dp, dsp));
}

#ifdef	PRINT_ATIP

#define	DOES(what,flag)	printf("  Does %s%s\n", flag?"":"not ",what);
#define	IS(what,flag)	printf("  Is %s%s\n", flag?"":"not ",what);
#define	VAL(what,val)	printf("  %s: %d\n", what, val[0]*256 + val[1]);
#define	SVAL(what,val)	printf("  %s: %s\n", what, val);

void
print_di(dip)
	struct disk_info	*dip;
{
static	char *ds_name[] = { "empty", "incomplete/appendable", "complete", "illegal" };
static	char *ss_name[] = { "empty", "incomplete/appendable", "illegal", "complete", };

	IS("erasable", dip->erasable);
	printf("disk status: %s\n", ds_name[dip->disk_status]);
	printf("session status: %s\n", ss_name[dip->sess_status]);
	printf("first track: %d number of sessions: %d first track in last sess: %d last track in last sess: %d\n",
		dip->first_track,
		dip->numsess,
		dip->first_track_ls,
		dip->last_track_ls);
	IS("unrestricted", dip->uru);
	printf("Disk type: ");
	switch (dip->disk_type) {

	case SES_DA_ROM:	printf("CD-DA or CD-ROM");	break;
	case SES_CDI:		printf("CDI");			break;
	case SES_XA:		printf("CD-ROM XA");		break;
	case SES_UNDEF:		printf("undefined");		break;
	default:		printf("reserved");		break;
	}
	printf("\n");
	if (dip->did_v)
		printf("Disk id: 0x%lX\n", a_to_u_long(dip->disk_id));

	printf("last start of lead in: %ld\n",
		msf_to_lba(dip->last_lead_in[1],
		dip->last_lead_in[2],
		dip->last_lead_in[3]));
	printf("last start of lead out: %ld\n",
		msf_to_lba(dip->last_lead_out[1],
		dip->last_lead_out[2],
		dip->last_lead_out[3]));

	if (dip->dbc_v)
		printf("Disk bar code: 0x%lX%lX\n",
			a_to_u_long(dip->disk_barcode),
			a_to_u_long(&dip->disk_barcode[4]));

	if (dip->num_opc_entries > 0) {
		printf("OPC table:\n");
	}
}

LOCAL	char	clv_to_speed[8] = {
		0, 2, 4, 6, 8, 0, 0, 0
};

void
print_atip(atp)
	struct atipinfo *atp;
{
	if (verbose)
		scsiprbytes("ATIP info: ", (Uchar *)atp, sizeof (*atp));

	printf("ATIP info from disk:\n");
	printf("  Indicated writing power: %d\n", atp->desc.ind_wr_power);
	if (atp->desc.erasable || atp->desc.ref_speed)
		printf("  Reference speed: %d\n", clv_to_speed[atp->desc.ref_speed]);
	IS("unrestricted", atp->desc.uru);
	IS("erasable", atp->desc.erasable);
	if (atp->desc.sub_type)
		printf("  Disk sub type: %d\n", atp->desc.sub_type);
	printf("  ATIP start of lead in:  %ld (%02d:%02d/%02d)\n",
		msf_to_lba(atp->desc.lead_in[1],
		atp->desc.lead_in[2],
		atp->desc.lead_in[3]),
		atp->desc.lead_in[1],
		atp->desc.lead_in[2],
		atp->desc.lead_in[3]);
	printf("  ATIP start of lead out: %ld (%02d:%02d/%02d)\n",
		msf_to_lba(atp->desc.lead_out[1],
		atp->desc.lead_out[2],
		atp->desc.lead_out[3]),
		atp->desc.lead_out[1],
		atp->desc.lead_out[2],
		atp->desc.lead_out[3]);
	if (atp->desc.a1_v) {
		if (atp->desc.clv_low != 0 || atp->desc.clv_high != 0)
			printf("  speed low: %d speed high: %d\n",
				clv_to_speed[atp->desc.clv_low],
				clv_to_speed[atp->desc.clv_high]);
		printf("  power mult factor: %d %d\n", atp->desc.power_mult, atp->desc.tgt_y_pow);
		if (atp->desc.erasable)
			printf("  recommended erase/write power: %d\n", atp->desc.rerase_pwr_ratio);
	}
	if (atp->desc.a2_v) {
		printf("  A2 values: %02X %02X %02X\n",
				atp->desc.a2[0],
				atp->desc.a2[1],
				atp->desc.a2[2]);
	}
	if (atp->desc.a3_v) {
		printf("  A3 values: %02X %02X %02X\n",
				atp->desc.a3[0],
				atp->desc.a3[1],
				atp->desc.a3[2]);
	}
}
#endif	/* PRINT_ATIP */

LOCAL int
speed_select_mmc(speed, dummy)
	int	speed;
	int	dummy;
{
	u_char	mode[0x100];
	int	len;
	struct	cd_mode_page_05 *mp;

	fillbytes((caddr_t)mode, sizeof(mode), '\0');

	if (!get_mode_params(0x05, "CD write parameter",
			mode, (u_char *)0, (u_char *)0, (u_char *)0, &len))
		return (-1);
	if (len == 0)
		return (-1);

	mp = (struct cd_mode_page_05 *)
		(mode + sizeof(struct scsi_mode_header) +
		((struct scsi_mode_header *)mode)->blockdesc_len);
#ifdef	DEBUG
	scsiprbytes("CD write parameter:", (u_char *)mode, len);
#endif


	mp->test_write = dummy != 0;
	/*
	 * Set default values:
	 * Write type = 01 (track at once)
	 * Track mode = 04 (CD-ROM)
	 * Data block type = 08 (CD-ROM)
	 * Session type = 00 (CD-ROM)
	 */
	mp->write_type = WT_TAO;
	mp->track_mode = TM_DATA; 
	mp->dbtype = DB_ROM_MODE1;
	mp->session_format = SES_DA_ROM;/* Matsushita has illegal def. value */


#ifdef	DEBUG
	scsiprbytes("CD write parameter:", (u_char *)mode, len);
#endif
	if (!set_mode_params("CD write parameter", mode, len, 0, -1))
		return (-1);

/*XXX max speed aus page 0x2A lesen !! */

/*	if (scsi_set_speed(-1, speed*176) < 0)*/

	/*
	 * 44100 * 2 * 2 =  176400 bytes/s
	 *
	 * The right formula would be:
	 * tmp = (((long)speed) * 1764) / 10;
	 *
	 * But the standard is rounding the wrong way.
	 * Furtunately rounding down is guaranteed.
	 */
	if (scsi_set_speed(-1, speed*177) < 0)
		return (-1);
	return (0);
}

#if defined(_BIT_FIELDS_LTOH)	/* Intel byteorder */

struct track_info {
	u_char	data_len[2];		/* Data len without this info	*/
	u_char	track_number;		/* Track number for this info	*/
	u_char	session_number;		/* Session number for this info	*/
	u_char	res4;			/* Reserved			*/
	Ucbit	track_mode	: 4;	/* Track mode (Q-sub control)	*/
	Ucbit	copy		: 1;	/* This track is a higher copy	*/
	Ucbit	damage		: 1;	/* if 1 & nwa_valid 0: inc track*/
	Ucbit	res5_67		: 2;	/* Reserved			*/
	Ucbit	data_mode	: 4;	/* Data mode of this track	*/
	Ucbit	fp		: 1;	/* This is a fixed packet track	*/
	Ucbit	packet		: 1;	/* This track is in packet mode	*/
	Ucbit	blank		: 1;	/* This is an invisible track	*/
	Ucbit	rt		: 1;	/* This is a reserved track	*/
	Ucbit	nwa_valid	: 1;	/* Next writable addr valid	*/
	Ucbit	res7_17		: 7;	/* Reserved			*/
	u_char	track_start[4];		/* Track start address		*/
	u_char	next_writable_addr[4];	/* Next writable address	*/
	u_char	free_blocks[4];		/* Free usr blocks in this track*/
	u_char	packet_size[4];		/* Packet size if in fixed mode	*/
	u_char	track_size[4];		/* # of user data blocks in trk	*/
};

#else				/* Motorola byteorder */

struct track_info {
	u_char	data_len[2];		/* Data len without this info	*/
	u_char	track_number;		/* Track number for this info	*/
	u_char	session_number;		/* Session number for this info	*/
	u_char	res4;			/* Reserved			*/
	Ucbit	res5_67		: 2;	/* Reserved			*/
	Ucbit	damage		: 1;	/* if 1 & nwa_valid 0: inc track*/
	Ucbit	copy		: 1;	/* This track is a higher copy	*/
	Ucbit	track_mode	: 4;	/* Track mode (Q-sub control)	*/
	Ucbit	rt		: 1;	/* This is a reserved track	*/
	Ucbit	blank		: 1;	/* This is an invisible track	*/
	Ucbit	packet		: 1;	/* This track is in packet mode	*/
	Ucbit	fp		: 1;	/* This is a fixed packet track	*/
	Ucbit	data_mode	: 4;	/* Data mode of this track	*/
	Ucbit	res7_17		: 7;	/* Reserved			*/
	Ucbit	nwa_valid	: 1;	/* Next writable addr valid	*/
	u_char	track_start[4];		/* Track start address		*/
	u_char	next_writable_addr[4];	/* Next writable address	*/
	u_char	free_blocks[4];		/* Free usr blocks in this track*/
	u_char	packet_size[4];		/* Packet size if in fixed mode	*/
	u_char	track_size[4];		/* # of user data blocks in trk	*/
};

#endif

LOCAL int
next_wr_addr_mmc(track, trackp, ap)
	int	track;
	track_t	*trackp;
	long	*ap;
{
	struct	track_info	track_info;
	long	next_addr;
	int	result = -1;


	/*
	 * Reading info for current track may require doing the read_track_info
	 * with either the track number (if the track is currently being written)
	 * or with 0xFF (if the track hasn't been started yet and is invisible
	 */

	if (track > 0 && is_packet(trackp)) {
		silent ++;
		result = read_track_info((caddr_t)&track_info, track,
							sizeof(track_info));
		silent --;
	}

	if (result < 0) {
		if (read_track_info((caddr_t)&track_info, 0xFF,
						sizeof(track_info)) < 0)
		return (-1);
	}
	if (verbose)
		scsiprbytes("track info:", (u_char *)&track_info,
				sizeof(track_info)-scsigetresid());
	next_addr = a_to_u_long(track_info.next_writable_addr);
	if (ap)
		*ap = next_addr;
	return (0);
}

int	st2mode[] = {
	0,		/* 0			*/
	TM_DATA,	/* 1 ST_ROM_MODE1	*/
	TM_DATA,	/* 2 ST_ROM_MODE2	*/
	0,		/* 3			*/
	0,		/* 4 ST_AUDIO_NOPRE	*/
	TM_PREEM,	/* 5 ST_AUDIO_PRE	*/
	0,		/* 6			*/
	0,		/* 7			*/
};

LOCAL int
open_track_mmc(dp, track, trackp)
	cdr_t	*dp;
	int	track;
	track_t *trackp;
{
	u_char	mode[0x100];
	int	len;
	struct	cd_mode_page_05 *mp;

	fillbytes((caddr_t)mode, sizeof(mode), '\0');

	if (!get_mode_params(0x05, "CD write parameter",
			mode, (u_char *)0, (u_char *)0, (u_char *)0, &len))
		return (-1);
	if (len == 0)
		return (-1);

	mp = (struct cd_mode_page_05 *)
		(mode + sizeof(struct scsi_mode_header) +
		((struct scsi_mode_header *)mode)->blockdesc_len);


/*	mp->track_mode = ???;*/
	mp->track_mode = st2mode[trackp->sectype & ST_MASK];
/*	mp->copy = ???;*/
	mp->dbtype = trackp->dbtype;

/*i_to_short(mp->audio_pause_len, 300);*/
/*i_to_short(mp->audio_pause_len, 150);*/
/*i_to_short(mp->audio_pause_len, 0);*/

	if (is_packet(trackp)) {
		mp->write_type = WT_PACKET;
		mp->track_mode |= TM_INCREMENTAL;
		mp->fp = (trackp->pktsize > 0) ? 1 : 0;
		i_to_4_byte(mp->packet_size, trackp->pktsize);
	} else {
		mp->write_type = WT_TAO;
		mp->fp = 0;
		i_to_4_byte(mp->packet_size, 0);
	}
 
#ifdef	DEBUG
	scsiprbytes("CD write parameter:", (u_char *)mode, len);
#endif
	if (!set_mode_params("CD write parameter", mode, len, 0, trackp->secsize))
		return (-1);

	return (0);
}

LOCAL int
close_track_mmc(track, trackp)
	int	track;
	track_t	*trackp;
{
	int	ret;

	if (scsi_flush_cache() < 0) {
		printf("Trouble flushing the cache\n");
		return -1;
	}
	wait_unit_ready(300);		/* XXX Wait for ATAPI */
	if (is_packet(trackp) && !is_noclose(trackp)) {
			/* close the incomplete track */
		ret = scsi_close_tr_session(1, 0xFF);
		wait_unit_ready(300);	/* XXX Wait for ATAPI */
		return (ret);
	}
	return (0);
}

int	toc2sess[] = {
	SES_DA_ROM,	/* CD-DA		 */
	SES_DA_ROM,	/* CD-ROM		 */
	SES_XA,		/* CD-ROM XA mode 1	 */
	SES_XA,		/* CD-ROM XA MODE 2	 */
	SES_CDI,	/* CDI			 */
	SES_DA_ROM,	/* Invalid - use default */
	SES_DA_ROM,	/* Invalid - use default */
};

LOCAL int
open_session_mmc(tracks, trackp, toctype, multi)
	int	tracks;
	track_t	*trackp;
	int	toctype;
	int	multi;
{
	u_char	mode[0x100];
	int	len;
	struct	cd_mode_page_05 *mp;

	fillbytes((caddr_t)mode, sizeof(mode), '\0');

	if (!get_mode_params(0x05, "CD write parameter",
			mode, (u_char *)0, (u_char *)0, (u_char *)0, &len))
		return (-1);
	if (len == 0)
		return (-1);

	mp = (struct cd_mode_page_05 *)
		(mode + sizeof(struct scsi_mode_header) +
		((struct scsi_mode_header *)mode)->blockdesc_len);

	mp->write_type = WT_TAO; /* fix to allow DAO later */

	mp->multi_session = (multi != 0) ? MS_MULTI : MS_NONE;
	mp->session_format = toc2sess[toctype & TOC_MASK];

#ifdef	DEBUG
	scsiprbytes("CD write parameter:", (u_char *)mode, len);
#endif
	if (!set_mode_params("CD write parameter", mode, len, 0, -1))
		return (-1);

	return (0);
}

LOCAL int
fixate_mmc(onp, dummy, toctype, tracks, trackp)
	int	onp;
	int	dummy;
	int	toctype;
	int	tracks;
	track_t	*trackp;
{
	int	ret;

	if (dummy && lverbose)
		printf("WARNING: Some drives don't like fixation in dummy mode.\n");
	ret = scsi_close_tr_session(2, 0);
	wait_unit_ready(300);		/* XXX Wait for ATAPI */
	return (ret);
}

char	*blank_types[] = {
	"entire disk",
	"PMA, TOC, pregap",
	"incomplete track",
	"reserved track",
	"tail of track",
	"closing of last session",
	"last session",
	"reserved blanking type",
};

LOCAL int
blank_mmc(addr, blanktype)
	long	addr;
	int	blanktype;
{
	BOOL	cdrr	 = FALSE;	/* Read CD-R	*/
	BOOL	cdwr	 = FALSE;	/* Write CD-R	*/
	BOOL	cdrrw	 = FALSE;	/* Read CD-RW	*/
	BOOL	cdwrw	 = FALSE;	/* Write CD-RW	*/

	mmc_check(&cdrr, &cdwr, &cdrrw, &cdwrw, NULL);
	if (!cdwrw)
		return (blank_dummy(addr, blanktype));

	if (lverbose)
		printf("Blanking %s\n", blank_types[blanktype & 0x07]);

	return (scsi_blank(addr, blanktype));
}
/***************************************************************************
 *            scsi-read-toc-pma-atip.c
 *
 *  Fri Oct 27 08:22:46 2006
 *  Copyright  2006  Rouquier Philippe
 *  <Rouquier Philippe@localhost.localdomain>
 ****************************************************************************/

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor Boston, MA 02110-1301,  USA
 */

#include <fcntl.h>

#include <glib.h>

#include "scsi-error.h"
#include "scsi-utils.h"
#include "scsi-base.h"
#include "scsi-command.h"
#include "scsi-opcodes.h"
#include "scsi-read-toc-pma-atip.h"

#if G_BYTE_ORDER == G_LITTLE_ENDIAN

struct _BraseroRdTocPmaAtipCDB {
	uchar opcode;

	uchar reserved0		:1;
	uchar msf		:1;
	uchar reserved1		:6;

	uchar format		:4;
	uchar reserved2		:4;

	uchar reserved3		[3];
	uchar track_session_num;

	uchar alloc_len		[2];
	uchar ctl;
};

#else

struct _BraseroRdTocPmaAtipCDB {
	uchar opcode;

	uchar reserved1		:6;
	uchar msf		:1;
	uchar reserved0		:1;

	uchar reserved2		:4;
	uchar format		:4;

	uchar reserved3		[3];
	uchar track_session_num;

	uchar alloc_len		[2];
	uchar ctl;
};

#endif

typedef struct _BraseroRdTocPmaAtipCDB BraseroRdTocPmaAtipCDB;

BRASERO_SCSI_COMMAND_DEFINE (BraseroRdTocPmaAtipCDB,
			     READ_TOC_PMA_ATIP,
			     O_RDONLY,
			     BRASERO_SCSI_READ);

typedef enum {
BRASERO_RD_TAP_FORMATTED_TOC		= 0x00,
BRASERO_RD_TAP_MULTISESSION_INFO	= 0x01,
BRASERO_RD_TAP_RAW_TOC			= 0x02,
BRASERO_RD_TAP_PMA			= 0x03,
BRASERO_RD_TAP_ATIP			= 0x04,
BRASERO_RD_TAP_CD_TEXT			= 0x05	/* Introduced in MMC3 */
} BraseroReadTocPmaAtipType;

static BraseroScsiResult
brasero_read_toc_pma_atip (BraseroRdTocPmaAtipCDB *cdb,
			   int desc_size,
			   BraseroScsiTocPmaAtipHdr **data,
			   int *size,
			   BraseroScsiErrCode *error)
{
	BraseroScsiTocPmaAtipHdr *buffer;
	BraseroScsiTocPmaAtipHdr hdr;
	BraseroScsiResult res;
	int request_size;
	int buffer_size;

	if (!data || !size) {
		BRASERO_SCSI_SET_ERRCODE (error, BRASERO_SCSI_BAD_ARGUMENT);
		return BRASERO_SCSI_FAILURE;
	}

	BRASERO_SET_16 (cdb->alloc_len, sizeof (BraseroScsiTocPmaAtipHdr));

	memset (&hdr, 0, sizeof (BraseroScsiTocPmaAtipHdr));
	res = brasero_scsi_command_issue_sync (cdb,
					       &hdr,
					       sizeof (BraseroScsiTocPmaAtipHdr),
					       error);
	if (res) {
		*size = 0;
		return res;
	}

	request_size = BRASERO_GET_16 (hdr.len) + sizeof (hdr.len);

	/* NOTE: if size is not valid use the maximum possible size */
	if ((request_size - sizeof (hdr)) % desc_size) {
		BRASERO_BURN_LOG ("Unaligned data (%i) setting to max (65530)", request_size);
		request_size = 65530;
	}
	else if (request_size - sizeof (hdr) < desc_size) {
		BRASERO_BURN_LOG ("Undersized data (%i) setting to max (65530)", request_size);
		request_size = 65530;
	}

	buffer = (BraseroScsiTocPmaAtipHdr *) g_new0 (uchar, request_size);

	BRASERO_SET_16 (cdb->alloc_len, request_size);
	res = brasero_scsi_command_issue_sync (cdb, buffer, request_size, error);
	if (res) {
		g_free (buffer);
		*size = 0;
		return res;
	}

	buffer_size = BRASERO_GET_16 (buffer->len) + sizeof (buffer->len);

	*data = buffer;
	*size = MIN (buffer_size, request_size);

	return res;
}

/**
 * Returns TOC data for all the sessions starting with track_num
 */

BraseroScsiResult
brasero_mmc1_read_toc_formatted (int fd,
				 int track_num,
				 BraseroScsiFormattedTocData **data,
				 int *size,
				 BraseroScsiErrCode *error)
{
	BraseroRdTocPmaAtipCDB *cdb;
	BraseroScsiResult res;

	cdb = brasero_scsi_command_new (&info, fd);
	cdb->format = BRASERO_RD_TAP_FORMATTED_TOC;
	cdb->track_session_num = track_num;

	res = brasero_read_toc_pma_atip (cdb,
					 sizeof (BraseroScsiTocDesc),
					(BraseroScsiTocPmaAtipHdr **) data,
					 size,
					 error);
	brasero_scsi_command_free (cdb);
	return res;
}

/**
 * Returns CD-TEXT information recorded in the leadin area as R-W sub-channel
 */

BraseroScsiResult
brasero_mmc3_read_cd_text (int fd,
			   BraseroScsiCDTextData **data,
			   int *size,
			   BraseroScsiErrCode *error)
{
	BraseroRdTocPmaAtipCDB *cdb;
	BraseroScsiResult res;

	cdb = brasero_scsi_command_new (&info, fd);
	cdb->format = BRASERO_RD_TAP_CD_TEXT;

	res = brasero_read_toc_pma_atip (cdb,
					 sizeof (BraseroScsiTocDesc),
					(BraseroScsiTocPmaAtipHdr **) data,
					 size,
					 error);
	brasero_scsi_command_free (cdb);
	return res;
}

/**
 * Returns ATIP information
 */

BraseroScsiResult
brasero_mmc1_read_atip (int fd,
			BraseroScsiAtipData **data,
			int *size,
			BraseroScsiErrCode *error)
{
	BraseroRdTocPmaAtipCDB *cdb;
	BraseroScsiResult res;

	/* In here we have to ask how many bytes the drive wants to return first
	 * indeed there is a difference in the descriptor size between MMC1/MMC2
	 * and MMC3. */
	cdb = brasero_scsi_command_new (&info, fd);
	cdb->format = BRASERO_RD_TAP_ATIP;
	cdb->msf = 1;				/* specs says it's compulsory */

	res = brasero_read_toc_pma_atip (cdb,
					 sizeof (BraseroScsiTocDesc),
					(BraseroScsiTocPmaAtipHdr **) data,
					 size,
					 error);
	brasero_scsi_command_free (cdb);
	return res;
}

/***************************************************************************
 *            burn-libburn-common.c
 *
 *  mer aoû 30 16:35:40 2006
 *  Copyright  2006  Rouquier Philippe
 *  bonfire-app@wanadoo.fr
 ***************************************************************************/

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>

#include "burn-basics.h"
#include "burn-job.h"
#include "burn-libburn-common.h"

#include <libburn/libburn.h>

void
brasero_libburn_common_ctx_free (BraseroLibburnCtx *ctx)
{
	if (ctx->drive_info) {
		burn_drive_info_free (ctx->drive_info);
		ctx->drive_info = NULL;
		ctx->drive = NULL;
	}

	if (ctx->disc) {
		burn_disc_free (ctx->disc);
		ctx->disc = NULL;
	}

	g_free (ctx);
}

BraseroLibburnCtx *
brasero_libburn_common_ctx_new (BraseroJob *job, GError **error)
{
	gchar libburn_device [BURN_DRIVE_ADR_LEN];
	BraseroLibburnCtx *ctx = NULL;
	gchar *device;
	int res;

	/* we just want to scan the drive proposed by NCB drive */
	brasero_job_get_device (job, &device);

	res = burn_drive_convert_fs_adr (device, libburn_device);
	g_free (device);

	if (res <= 0) {
		g_set_error (error,
			     BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_GENERAL,
			     _("the drive address couldn't be retrieved"));
		return NULL;
	}

	ctx = g_new0 (BraseroLibburnCtx, 1);
	res = burn_drive_scan_and_grab (&ctx->drive_info, libburn_device, 0);
	if (res <= 0) {
		g_free (ctx);
		g_set_error (error,
			     BRASERO_BURN_ERROR,
			     BRASERO_BURN_ERROR_GENERAL,
			     _("the drive couldn't be initialized"));
		return NULL;
	}

	ctx->drive = ctx->drive_info->drive;
	return ctx;	
}

static gboolean
brasero_libburn_common_process_message (BraseroJob *self)
{
	int ret;
	GError *error;
	int err_code = 0;
	int err_errno = 0;
	char err_sev [80];
	char err_txt [BURN_MSGS_MESSAGE_LEN] = {0};

	ret = burn_msgs_obtain ("FATAL",
				&err_code,
				err_txt,
				&err_errno,
				err_sev);

	if (ret == 0)
	        return TRUE;

	if (ret < 0) {
		error = g_error_new (BRASERO_BURN_ERROR,
				     BRASERO_BURN_ERROR_GENERAL,
				     err_txt);
		brasero_job_error (BRASERO_JOB (self), error);
		return FALSE;
	}

	BRASERO_JOB_LOG (self,
			 _("(%s) libburn tried to say something"),
		         err_txt);
	return TRUE;
}

static void
brasero_libburn_common_status_changed (BraseroJob *self,
				       BraseroLibburnCtx *ctx,
				       enum burn_drive_status status,
				       struct burn_progress *progress)
{
	BraseroBurnAction action = BRASERO_BURN_ACTION_NONE;

	switch (status) {
		case BURN_DRIVE_WRITING:
			/* we ignore it if it happens after leadout */
			if (ctx->status == BURN_DRIVE_WRITING_LEADOUT
			||  ctx->status == BURN_DRIVE_CLOSING_TRACK)
				return;

			if (ctx->status == BURN_DRIVE_WRITING_LEADIN
			||  ctx->status == BURN_DRIVE_WRITING_PREGAP) {
				ctx->sectors += ctx->track_sectors;
				ctx->track_sectors = progress->sectors;
				ctx->track_num = progress->track;
			}

			action = BRASERO_BURN_ACTION_RECORDING;
			brasero_job_set_dangerous (BRASERO_JOB (self), TRUE);
			break;

		case BURN_DRIVE_WRITING_LEADIN:		/* DAO */
		case BURN_DRIVE_WRITING_PREGAP:		/* TAO */
			ctx->has_leadin = 1;
			action = BRASERO_BURN_ACTION_START_RECORDING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_WRITING_LEADOUT: 	/* DAO */
		case BURN_DRIVE_CLOSING_TRACK:		/* TAO */
			ctx->sectors += ctx->track_sectors;
			ctx->track_sectors = progress->sectors;

			action = BRASERO_BURN_ACTION_FIXATING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_ERASING:
		case BURN_DRIVE_FORMATTING:
			action = BRASERO_BURN_ACTION_BLANKING;
			brasero_job_set_dangerous (BRASERO_JOB (self), TRUE);
			break;

		case BURN_DRIVE_IDLE:
			/* FIXME: that's where a track is returned */
			brasero_job_finished_session (BRASERO_JOB (self));
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_SPAWNING:
			if (ctx->status == BURN_DRIVE_IDLE)
				action = BRASERO_BURN_ACTION_START_RECORDING;
			else
				action = BRASERO_BURN_ACTION_FIXATING;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		case BURN_DRIVE_READING:
			action = BRASERO_BURN_ACTION_DRIVE_COPY;
			brasero_job_set_dangerous (BRASERO_JOB (self), FALSE);
			break;

		default:
			return;
	}

	ctx->status = status;
	brasero_job_set_current_action (self,
					action,
					NULL,
					FALSE);
}

void
brasero_libburn_common_clock_id (BraseroJob *self,
				 BraseroLibburnCtx *ctx)
{
	enum burn_drive_status status;
	struct burn_progress progress;

	/* see if there is any pending message */
	if (!brasero_libburn_common_process_message (self))
		return;

	if (!ctx->drive)
		return;

	status = burn_drive_get_status (ctx->drive, &progress);

	/* FIXME! for some operations that libburn can't perform the drive stays
	 * idle and we've got no way to tell that kind of use case */
	if (ctx->status != status)
		brasero_libburn_common_status_changed (self, ctx, status, &progress);

	if (status == BURN_DRIVE_IDLE
	||  status == BURN_DRIVE_SPAWNING
	||  !progress.sectors
	||  !progress.sector) {
		ctx->sectors = 0;

		ctx->track_num = progress.track;
		ctx->track_sectors = progress.sectors;
		return;
	}

	if (status != BURN_DRIVE_ERASING
	&&  status != BURN_DRIVE_FORMATTING) {
		gint64 cur_sector;

		if (ctx->track_num != progress.track) {
			gchar *string;

			ctx->sectors += ctx->track_sectors;
			ctx->track_sectors = progress.sectors;
			ctx->track_num = progress.track;

			string = g_strdup_printf (_("Writing track %02i"), progress.track);
			brasero_job_set_current_action (self,
							BRASERO_BURN_ACTION_RECORDING,
							string,
							TRUE);
			g_free (string);
		}

		cur_sector = progress.sector + ctx->sectors;
		brasero_job_set_written_session (self, cur_sector * 2048);
	}
	else {
		gdouble fraction;

		/* when erasing only set progress */
		fraction = (gdouble) (progress.sector) /
			   (gdouble) (progress.sectors);

		brasero_job_set_progress (self, fraction);
	}

	brasero_job_start_progress (self, FALSE);
}

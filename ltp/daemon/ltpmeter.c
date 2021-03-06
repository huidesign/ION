/*

	ltpmeter.c:	LTP flow control and block segmentation daemon.

	Author: Scott Burleigh, JPL

	Copyright (c) 2007, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship acknowledged.
	
									*/
#include "ltpP.h"

#if defined (VXWORKS) || defined (RTEMS)
int	ltpmeter(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	unsigned long	remoteEngineId =
				a1 == 0 ? 0 : strtoul((char *) a1, NULL, 0);
#else
int	main(int argc, char *argv[])
{
	unsigned long	remoteEngineId =
				argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
#endif
	Sdr		sdr;
	LtpVdb		*vdb;
	LtpVspan	*vspan;
	PsmAddress	vspanElt;
	Object		spanObj;
	LtpSpan		span;
	int		returnCode = 0;
	char		memo[64];
	ExportSession	session;
	Lyst		extents;
	ExportExtent	*extent;
	int		segmentsIssued;

	if (remoteEngineId == 0)
	{
		PUTS("Usage: ltpmeter <non-zero remote engine ID>");
		return 0;
	}

	if (ltpInit(0, 0) < 0)
	{
		putErrmsg("ltpmeter can't initialize LTP.",
				utoa(remoteEngineId));
		return 1;
	}

	sdr = getIonsdr();
	vdb = getLtpVdb();
	sdr_begin_xn(sdr);
	findSpan(remoteEngineId, &vspan, &vspanElt);
	if (vspanElt == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No such engine in database.", itoa(remoteEngineId));
		return 1;
	}

	if (vspan->meterPid > 0 && vspan->meterPid != sm_TaskIdSelf())
	{
		sdr_exit_xn(sdr);
		putErrmsg("ltpmeter task is already started for this engine.",
				itoa(remoteEngineId));
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	spanObj = sdr_list_data(sdr, vspan->spanElt);
	sdr_stage(sdr, (char *) &span, spanObj, sizeof(LtpSpan));
	if (span.currentExportSessionObj == 0)	/*	New span.	*/
	{
		/*	Must start span's initial session.		*/

		sdr_exit_xn(sdr);
		if (startExportSession(sdr, spanObj, vspan) < 0)
		{
			putErrmsg("ltpmeter can't start new session.",
					itoa(remoteEngineId));
			return 1;
		}

		sdr_begin_xn(sdr);
		sdr_stage(sdr, (char *) &span, spanObj, sizeof(LtpSpan));
	}

	writeMemo("[i] ltpmeter is running.");
	while (1)
	{
		/*	First wait until block aggregation buffer for
		 *	this span is full.				*/

		if (span.lengthOfBufferedBlock < span.aggrSizeLimit)
		{
			sdr_exit_xn(sdr);
			if (sm_SemTake(vspan->bufFullSemaphore) < 0)
			{
				putErrmsg("Can't take bufFullSemaphore.",
						itoa(remoteEngineId));
				returnCode = 1;
				break;		/*	Outer loop.	*/
			}

			if (sm_SemEnded(vspan->bufFullSemaphore))
			{
				isprintf(memo, sizeof memo, "[i] LTP meter to \
engine %lu is stopped.", remoteEngineId);
				writeMemo(memo);
				break;		/*	Outer loop.	*/
			}

			sdr_begin_xn(sdr);
			sdr_stage(sdr, (char *) &span, spanObj,
					sizeof(LtpSpan));
		}

		if (span.lengthOfBufferedBlock == 0)
		{
			continue;	/*	Nothing to do yet.	*/
		}

		/*	Now segment the block that is currently
		 *	aggregated in the buffer, giving the span's
		 *	segSemaphore once per segment.			*/

		sdr_stage(sdr, (char *) &session, span.currentExportSessionObj,
				sizeof(ExportSession));
		session.clientSvcId = span.clientSvcIdOfBufferedBlock;
		encodeSdnv(&(session.clientSvcIdSdnv), session.clientSvcId);
		session.totalLength = span.lengthOfBufferedBlock;
		session.redPartLength = span.redLengthOfBufferedBlock;
		if ((extents = lyst_create_using(getIonMemoryMgr())) == NULL
		|| (extent = (ExportExtent *) MTAKE(sizeof(ExportExtent)))
				== NULL
		|| lyst_insert_last(extents, extent) == NULL)
		{
			putErrmsg("Can't create extents list.", NULL);
			sdr_cancel_xn(sdr);
			returnCode = 1;
			break;			/*	Outer loop.	*/
		}

		extent->offset = 0;
		extent->length = session.totalLength;
		segmentsIssued = issueSegments(sdr, &span, vspan, &session,
				span.currentExportSessionObj, extents, 0);
		MRELEASE(extent);
		lyst_destroy(extents);
		switch (segmentsIssued)
		{
		case -1:		/*	System error.		*/
			putErrmsg("Can't segment block.", NULL);
			sdr_cancel_xn(sdr);
			returnCode = 1;
			break;			/*	Outer loop.	*/

		case 0:			/*	Database too full.	*/
			sdr_cancel_xn(sdr);

			/*	Wait one second and try again.		*/

			snooze(1);
			sdr_begin_xn(sdr);
			sdr_stage(sdr, (char *) &span, spanObj,
					sizeof(LtpSpan));
			continue;
		}

		/*	Segment issuance succeeded.			*/

		if (vdb->watching & WATCH_f)
		{
			putchar('f');
			fflush(stdout);
		}

		/*	Commit changes to current session to the
		 *	database.					*/

		sdr_write(sdr, span.currentExportSessionObj, (char *) &session,
				sizeof(ExportSession));

		/*	Reinitialize span's block buffer.		*/

		span.ageOfBufferedBlock = 0;
		span.lengthOfBufferedBlock = 0;
		span.redLengthOfBufferedBlock = 0;
		span.clientSvcIdOfBufferedBlock = 0;
		span.currentExportSessionObj = 0;
		sdr_write(sdr, spanObj, (char *) &span, sizeof(LtpSpan));
		if (sdr_end_xn(sdr))
		{
			putErrmsg("Can't finish session.", NULL);
			returnCode = 1;
			break;			/*	Outer loop.	*/
		}

		/*	Wait until window opens enabling start of next
		 *	session.					*/

		while (sdr_list_length(sdr, span.exportSessions)
				>= span.maxExportSessions)
		{
			if (sm_SemTake(vdb->sessionSemaphore) < 0)
			{
				putErrmsg("Can't take sessionSemaphore.", NULL);
				returnCode = 1;
				break;		/*	Inner loop.	*/
			}

			if (sm_SemEnded(vdb->sessionSemaphore))
			{
				putErrmsg("LTP has been stopped.", NULL);
				returnCode = 1;
				break;		/*	Inner loop.	*/
			}
		}

		if (returnCode != 0)
		{
			break;			/*	Outer loop.	*/
		}

		/*	Start an export session for the next block.	*/

		if (startExportSession(sdr, spanObj, vspan) < 0)
		{
			putErrmsg("ltpmeter can't start new session.",
					utoa(remoteEngineId));
			returnCode = 1;
			break;			/*	Outer loop.	*/
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();

		/*	Now start next cycle of main loop, waiting
		 *	for the new session's buffer to be filled.	*/

		sdr_begin_xn(sdr);
		sdr_stage(sdr, (char *) &span, spanObj, sizeof(LtpSpan));
	}

	writeErrmsgMemos();
	writeMemo("[i] ltpmeter has ended.");
	ionDetach();
	return returnCode;
}

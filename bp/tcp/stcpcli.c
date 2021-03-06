/*
	stcpcli.c:	BP TCP-based convergence-layer input
			daemon, designed to serve as an input
			duct.

	Author: Scott Burleigh, JPL

	Copyright (c) 2004, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "tcpcla.h"

static pthread_t	stcpcliMainThread(pthread_t tid)
{
	static pthread_t	mainThread = 0;

	if (tid)
	{
		mainThread = tid;
	}

	return mainThread;
}

static void	interruptThread()
{
	pthread_t	mainThread = stcpcliMainThread(0);

	isignal(SIGTERM, interruptThread);
	if (mainThread != pthread_self())
	{
		pthread_kill(mainThread, SIGTERM);
	}
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	char		senderEidBuffer[SDRSTRING_BUFSZ];
	char		*senderEid;
	VInduct		*vduct;
	LystElt		elt;
	pthread_mutex_t	*mutex;
	int		bundleSocket;
	pthread_t	thread;
} ReceiverThreadParms;

static void	terminateReceiverThread(ReceiverThreadParms *parms)
{
	writeErrmsgMemos();
	writeMemo("[i] stcpcli receiver thread stopping.");
	pthread_mutex_lock(parms->mutex);
	if (parms->bundleSocket != -1)
	{
		close(parms->bundleSocket);
		parms->bundleSocket = -1;
	}

	lyst_delete(parms->elt);
	pthread_mutex_unlock(parms->mutex);
}

static void	*receiveBundles(void *parm)
{
	/*	Main loop for bundle reception thread on one
	 *	connection, terminating when connection is lost.	*/

	ReceiverThreadParms	*parms = (ReceiverThreadParms *) parm;
	AcqWorkArea		*work;
	char			*buffer;
	int			threadRunning = 1;

	work = bpGetAcqArea(parms->vduct);
	if (work == NULL)
	{
		putErrmsg("stcpcli can't get acquisition work area.", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("stcpcli can't get TCP buffer.", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms);
		return NULL;
	}

	/*	Now start receiving bundles.				*/

	while (threadRunning)
	{
		if (bpBeginAcq(work, 0, parms->senderEid) < 0)
		{
			putErrmsg("Can't begin acquisition of bundle.", NULL);
			threadRunning = 0;
			continue;
		}

		switch (receiveBundleByTcp(parms->bundleSocket, work, buffer))
		{
		case -1:
			putErrmsg("Can't acquire bundle.", NULL);

			/*	Intentional fall-through to next case.	*/

		case 0:				/*	Normal stop.	*/
			threadRunning = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		if (bpEndAcq(work) < 0)
		{
			putErrmsg("Can't end acquisition of bundle.", NULL);
			threadRunning = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	End of receiver thread; release resources.		*/

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	terminateReceiverThread(parms);
	MRELEASE(parms);
	return NULL;
}

/*	*	*	Access thread functions	*	*	*	*/

typedef struct
{
	VInduct			*vduct;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	int			ductSocket;
	int			running;
} AccessThreadParms;

static void	*spawnReceivers(void *parm)
{
	/*	Main loop for acceptance of connections and
	 *	creation of receivers to service those connections.	*/

	AccessThreadParms	*atp = (AccessThreadParms *) parm;
	pthread_mutex_t		mutex;
	Lyst			threads;
	int			newSocket;
	struct sockaddr		cloSocketName;
	socklen_t		nameLength;
	ReceiverThreadParms	*parms;
	LystElt			elt;
	struct sockaddr_in	*fromAddr;
	unsigned int		hostNbr;
	char			hostName[MAXHOSTNAMELEN + 1];
	pthread_t		thread;

	pthread_mutex_init(&mutex, NULL);
	threads = lyst_create_using(getIonMemoryMgr());
	if (threads == NULL)
	{
		putErrmsg("stcpcli can't create threads list.", NULL);
		pthread_mutex_destroy(&mutex);
		return NULL;
	}

	/*	Can now begin accepting connections from remote
	 *	contacts.  On failure, take down the whole CLI.		*/

	while (1)
	{
		nameLength = sizeof(struct sockaddr);
		newSocket = accept(atp->ductSocket, &cloSocketName,
				&nameLength);
		if (newSocket < 0)
		{
			putSysErrmsg("stcpcli accept() failed", NULL);
			pthread_kill(stcpcliMainThread(0), SIGTERM);
			continue;
		}

		if (atp->running == 0)
		{
			break;	/*	Main thread has shut down.	*/
		}

		parms = (ReceiverThreadParms *)
				MTAKE(sizeof(ReceiverThreadParms));
		if (parms == NULL)
		{
			putErrmsg("stcpcli can't allocate for new thread.",
					NULL);
			pthread_kill(stcpcliMainThread(0), SIGTERM);
			continue;
		}

		parms->vduct = atp->vduct;
		pthread_mutex_lock(&mutex);
		parms->elt = lyst_insert_last(threads, parms);
		pthread_mutex_unlock(&mutex);
		if (parms->elt == NULL)
		{
			putErrmsg("stcpcli can't allocate for new thread.",
					NULL);
			MRELEASE(parms);
			pthread_kill(stcpcliMainThread(0), SIGTERM);
			continue;
		}

		parms->mutex = &mutex;
		parms->bundleSocket = newSocket;
       		fromAddr = (struct sockaddr_in *) &cloSocketName;
		memcpy((char *) &hostNbr,
				(char *) &(fromAddr->sin_addr.s_addr), 4);
		hostNbr = ntohl(hostNbr);
		if (getInternetHostName(hostNbr, hostName))
		{
			parms->senderEid = parms->senderEidBuffer;
			getSenderEid(&(parms->senderEid), hostName);
		}
		else
		{
			parms->senderEid = NULL;
		}
	
		if (pthread_create(&(parms->thread), NULL, receiveBundles,
					parms))
		{
			putSysErrmsg("stcpcli can't create new thread", NULL);
			MRELEASE(parms);
			pthread_kill(stcpcliMainThread(0), SIGTERM);
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	close(atp->ductSocket);
	writeErrmsgMemos();

	/*	Shut down all current CLI threads cleanly.		*/

	while (1)
	{
		pthread_mutex_lock(&mutex);
		elt = lyst_first(threads);
		if (elt == NULL)	/*	All threads shut down.	*/
		{
			pthread_mutex_unlock(&mutex);
			break;
		}

		/*	Trigger termination of thread.			*/

		parms = (ReceiverThreadParms *) lyst_data(elt);
		pthread_mutex_unlock(&mutex);
		thread = parms->thread;
		pthread_kill(thread, SIGTERM);
		pthread_join(thread, NULL);
		close(parms->bundleSocket);
	}

	lyst_destroy(threads);
	writeErrmsgMemos();
	writeMemo("[i] stcpcli access thread has ended.");
	pthread_mutex_destroy(&mutex);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (VXWORKS) || defined (RTEMS)
int	stcpcli(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Induct			duct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	AccessThreadParms	atp;
	socklen_t		nameLength;
	char			*tcpDelayString;
	pthread_t		accessThread;
	int			fd;

	if (ductName == NULL)
	{
		PUTS("Usage: stcpcli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("stcpcli can't attach to BP.", NULL);
		return -1;
	}

	findInduct("stcp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such stcp duct.", ductName);
		return -1;
	}

	if (vduct->cliPid > 0 && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return -1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	if (protocol.nominalRate <= 0)
	{
		vduct->acqThrottle.nominalRate = DEFAULT_TCP_RATE;
	}
	else
	{
		vduct->acqThrottle.nominalRate = protocol.nominalRate;
	}

	hostName = ductName;
	parseSocketSpec(ductName, &portNbr, &hostNbr);
	if (portNbr == 0)
	{
		portNbr = BpTcpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	if (hostNbr == 0)
	{
		putErrmsg("Can't get IP address for host.", hostName);
		return -1;
	}

	hostNbr = htonl(hostNbr);
	atp.vduct = vduct;
	memset((char *) &(atp.socketName), 0, sizeof(struct sockaddr));
	atp.inetName = (struct sockaddr_in *) &(atp.socketName);
	atp.inetName->sin_family = AF_INET;
	atp.inetName->sin_port = portNbr;
	memcpy((char *) &(atp.inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	atp.ductSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (atp.ductSocket < 0)
	{
		putSysErrmsg("Can't open TCP socket", NULL);
		return 1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(atp.ductSocket)
	|| bind(atp.ductSocket, &(atp.socketName), nameLength) < 0
	|| listen(atp.ductSocket, 5) < 0
	|| getsockname(atp.ductSocket, &(atp.socketName), &nameLength) < 0)
	{
		close(atp.ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return 1;
	}

	tcpDelayString = getenv("TCP_DELAY_NSEC_PER_BYTE");
	if (tcpDelayString == NULL)
	{
		tcpDelayEnabled = 0;
	}
	else
	{
		tcpDelayEnabled = 1;
		tcpDelayNsecPerByte = strtol(tcpDelayString, NULL, 0);
		if (tcpDelayNsecPerByte < 0
		|| tcpDelayNsecPerByte > 16384)
		{
			tcpDelayNsecPerByte = 0;
		}
	}

	/*	Initialize sender endpoint ID lookup.			*/

	ipnInit();
	dtn2Init();

	/*	Set up signal handling: SIGTERM is shutdown signal.	*/

	oK(stcpcliMainThread(pthread_self()));
	isignal(SIGTERM, interruptThread);

	/*	Start the access thread.				*/

	atp.running = 1;
	if (pthread_create(&accessThread, NULL, spawnReceivers, &atp))
	{
		close(atp.ductSocket);
		putSysErrmsg("stcpcli can't create access thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the induct.				*/

	writeMemo("[i] stcpcli is running.");
	snooze(2000000000);

	/*	Time to shut down.					*/

	atp.running = 0;

	/*	Wake up the access thread by connecting to it.		*/

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd >= 0)
	{
		connect(fd, &(atp.socketName), sizeof(struct sockaddr));

		/*	Immediately discard the connected socket.	*/

		close(fd);
	}

	pthread_join(accessThread, NULL);
	writeErrmsgMemos();
	writeMemo("[i] stcpcli duct has ended.");
	ionDetach();
	return 0;
}

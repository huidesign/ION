/*
	tcpclo.c:	BP TCP-based convergence-layer output
			daemon.  Note that this convergence-layer
			output daemon is a "dedicated" CLO daemon
			suitable only for a limited number of paths,
			because it manages just a single TCP
			connection.

			Promiscuous CLO daemons need to be based on
			UDP, Dgr, etc.
			
			Modification : This has been made compliant to draft-irtf-dtnrg-tcp-clayer-02.

	Author: Scott Burleigh, JPL

	Copyright (c) 2004, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "tcpcla.h"

static sm_SemId		tcpcloSemaphore;

static void	shutDownClo()	/*	Commands CLO termination.	*/
{
	sm_SemEnd(tcpcloSemaphore);
}

/*	*	*	Keepalive thread functions	*	*	*/

typedef struct
{
	int		*cloRunning;
	pthread_mutex_t	*mutex;
	struct sockaddr	*socketName;
	int		*ductSocket;
	int		*keepalivePeriod;
} KeepaliveThreadParms;

static void	*sendKeepalives(void *parm)
{
	KeepaliveThreadParms	*parms = (KeepaliveThreadParms *) parm;
	int			count = 0;
	int			bytesSent;
	int			backoffTimer = BACKOFF_TIMER_START;
	int 			backoffTimerCount = 0;
	unsigned char 		*buffer;

	buffer = MTAKE(TCPCLA_BUFSZ);	//To send keepalive bundle
	if (buffer == NULL)
	{
		putErrmsg("No memory for TCP buffer in tcpclo.", NULL);
		return NULL;
	}

	iblock(SIGTERM);
	while (*(parms->cloRunning))
	{
		snooze(1);
		count++;
		if (count < *(parms->keepalivePeriod))
		{
			continue;
		}

		// If the negotiated keep alive interval is 0, then
		// keep alives will not be sent.
		if(*(parms->keepalivePeriod) == 0)
		{
			continue;
		}

		/*	Time to send a keepalive.  Note that the
		 *	interval between keepalive attempts will be
		 *	KEEPALIVE_PERIOD plus (if the remote induct
		 *	is not reachable) the length of time taken
		 *	by TCP to determine that the connection
		 *	attempt will not succeed (e.g., 3 seconds).	*/

		count = 0;
		pthread_mutex_lock(parms->mutex);
		bytesSent = sendBundleByTCPCL(parms->socketName,
				parms->ductSocket, 0, 0, buffer, parms->keepalivePeriod);
		pthread_mutex_unlock(parms->mutex);
		/*	if the node is unable to establish a TCP connection,
 		 * 	the connection should be tried only after some delay.
 		 *								*/
		if(bytesSent == 0)
		{	
			while((backoffTimerCount < backoffTimer) && (*(parms->ductSocket) < 0))
			{
				snooze(1);
				backoffTimerCount++;
				if(!(*(parms->cloRunning)))
				{
					break;
				}
			}
			backoffTimerCount = 0;
			if(backoffTimer < BACKOFF_TIMER_LIMIT)
			{
				backoffTimer *= 2;
			}
			continue;
		}
		backoffTimer = BACKOFF_TIMER_START;
		if (bytesSent < 0)
		{
			shutDownClo();
			break;
		}
	}
	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Receiver thread functions	*	*	*/
typedef struct
{
	int		*cloRunning;
	pthread_mutex_t	*mutex;
	int		*bundleSocket;
	VInduct		*vduct;
} ReceiveThreadParms;


static void	*receiveBundles(void *parm)
{
	/*	Main loop for bundle reception thread	*/

	ReceiveThreadParms	*parms = (ReceiveThreadParms *) parm;
	int			threadRunning = 1;
	AcqWorkArea		*work;
	char			*buffer;

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putSysErrmsg("tcpclo receiver can't get TCP buffer", NULL);
		return NULL;
	}

	work = bpGetAcqArea(parms->vduct);
	if (work == NULL)
	{
		putSysErrmsg("tcpclo receiver can't get acquisition work area",
				NULL);
		MRELEASE(buffer);
		return NULL;
	}

	iblock(SIGTERM);
	while (threadRunning && *(parms->cloRunning))
	{
		snooze(1);
		if(*(parms->bundleSocket) < 0)
		{
			threadRunning = 0;
			continue;
		}

		if (bpBeginAcq(work, 0, NULL) < 0)
		{
			putErrmsg("Can't begin acquisition of bundle.", NULL);
			threadRunning = 0;
			continue;
		}
	
		switch (receiveBundleByTcpCL(*(parms->bundleSocket), work,
					buffer))
		{
		case -1:
			putErrmsg("Can't acquire bundle.", NULL);
			/*	Intentional fall-through to next case.	*/

		case 0:			/*	Shutdown message	*/	
			/*	Go back to the start of the while loop	*/
			pthread_mutex_lock(parms->mutex);
			close(*(parms->bundleSocket));
			*(parms->bundleSocket) = -1;
			pthread_mutex_unlock(parms->mutex);			
			threadRunning = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		if (bpEndAcq(work) < 0)
		{
			putErrmsg("Can't end acquisition of bundle.", NULL);
			threadRunning = 0;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	End of receiver thread; release resources.		*/

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (VXWORKS) || defined (RTEMS)
int	tcpclo(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	unsigned char		*buffer;
	VOutduct		*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Outduct			duct;
	ClProtocol		protocol;
	Outflow			outflows[3];
	int			i;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	int			running = 1;
	pthread_mutex_t		mutex;
	KeepaliveThreadParms	parms;
	ReceiveThreadParms	rparms;
	pthread_t		keepaliveThread;
	pthread_t		receiverThread;
	Object			bundleZco;
	BpExtendedCOS		extendedCOS;
	char			destDuctName[MAX_CL_DUCT_NAME_LEN + 1];
	unsigned int		bundleLength;
	int			ductSocket = -1;
	int			bytesSent;
	int 			keepalivePeriod = 0;
	VInduct			*viduct;

	if (ductName == NULL)
	{
		PUTS("Usage: tcpclo <remote host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putSysErrmsg("tcpclo can't attach to BP", NULL);
		return 1;
	}

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("No memory for TCP buffer in tcpclo.", NULL);
		return 1;
	}

	findOutduct("tcp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such tcp duct.", ductName);
		MRELEASE(buffer);
		return 1;
	}

	if (vduct->cloPid > 0 && vduct->cloPid != sm_TaskIdSelf())
	{
		putErrmsg("CLO task is already started for this duct.",
				itoa(vduct->cloPid));
		MRELEASE(buffer);
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->outductElt),
			sizeof(Outduct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	if (protocol.nominalRate <= 0)
	{
		vduct->xmitThrottle.nominalRate = DEFAULT_TCP_RATE;
	}
	else
	{
		vduct->xmitThrottle.nominalRate = protocol.nominalRate;
	}

	memset((char *) outflows, 0, sizeof outflows);
	outflows[0].outboundBundles = duct.bulkQueue;
	outflows[1].outboundBundles = duct.stdQueue;
	outflows[2].outboundBundles = duct.urgentQueue;
	for (i = 0; i < 3; i++)
	{
		outflows[i].svcFactor = 1 << i;
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
		putSysErrmsg("Can't get IP address for host.", hostName);
		MRELEASE(buffer);
		return 1;
	}

	hostNbr = htonl(hostNbr);
	memset((char *) &socketName, 0, sizeof socketName);
	inetName = (struct sockaddr_in *) &socketName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &hostNbr, 4);

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	tcpcloSemaphore = vduct->semaphore;
	sm_TaskVarAdd(&tcpcloSemaphore);
	isignal(SIGTERM, shutDownClo);
	isignal(SIGPIPE, handleConnectionLoss);

	/*	Start the keepalive thread for the eventual connection.	*/
	
	tcpDesiredKeepAlivePeriod = KEEPALIVE_PERIOD;
	parms.cloRunning = &running;
	pthread_mutex_init(&mutex, NULL);
	parms.mutex = &mutex;
	parms.socketName = &socketName;
	parms.ductSocket = &ductSocket;
	parms.keepalivePeriod = &keepalivePeriod;
	if (pthread_create(&keepaliveThread, NULL, sendKeepalives, &parms))
	{
		putSysErrmsg("tcpclo can't create keepalive thread", NULL);
		MRELEASE(buffer);
		pthread_mutex_destroy(&mutex);
		return 1;
	}

	// Returns the VInduct Object of first induct with same protocol
	// as the outduct. The VInduct is required to create an acq area.
	// The Acq Area inturn uses the throttle information from VInduct
	// object while receiving bundles. The throttle information 
	// of all inducts of the same induct will be the same, so choosing 
	// any induct will serve the purpose.
	
	findVInduct(&viduct,protocol.name);
	if(viduct == NULL)
	{
		putSysErrmsg("tcpclo can't get VInduct", NULL);
		MRELEASE(buffer);
		pthread_mutex_destroy(&mutex);
		return 1;
	
	}

	rparms.vduct =  viduct;
	rparms.bundleSocket = &ductSocket;
	rparms.mutex = &mutex;
	rparms.cloRunning = &running;
	if (pthread_create(&receiverThread, NULL, receiveBundles, &rparms))
	{
		putSysErrmsg("tcpclo can't create receive thread", NULL);
		MRELEASE(buffer);
		pthread_mutex_destroy(&mutex);
		return 1;
	}

	/*	Can now begin transmitting to remote duct.		*/

	writeMemo("[i] tcpclo is running.");
	while (running && !(sm_SemEnded(tcpcloSemaphore)))
	{
		if (bpDequeue(vduct, outflows, &bundleZco, &extendedCOS,
				destDuctName) < 0)
		{
			running = 0;	/*	Terminate CLO.		*/
			continue;
		}

		if (bundleZco == 0)	/*	Interrupted.		*/
		{
			continue;
		}

		bundleLength = zco_length(sdr, bundleZco);
		pthread_mutex_lock(&mutex);
		bytesSent = sendBundleByTCPCL(&socketName, &ductSocket,
				bundleLength, bundleZco, buffer,&keepalivePeriod);
		pthread_mutex_unlock(&mutex);
		if(bytesSent < 0)
		{
			running = 0;	/*	Terminate CLO.		*/
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}
	if( sendShutDownMessage(&ductSocket, SHUT_DN_NO, -1) < 0)
	{
		putErrmsg("Sending Shutdown message failed!!",NULL);
	}

	if (ductSocket != -1)
	{
		close(ductSocket);
	}

	pthread_join(keepaliveThread, NULL);
	writeMemo("tcpclo keep alive thread killed");
	running = 0;
	
	pthread_join(receiverThread, NULL);
	writeMemo("tcpclo receiver thread killed");

	writeErrmsgMemos();
	writeMemo("[i] tcpclo duct has ended.");
	MRELEASE(buffer);
	pthread_mutex_destroy(&mutex);
	bp_detach();
	return 0;
}

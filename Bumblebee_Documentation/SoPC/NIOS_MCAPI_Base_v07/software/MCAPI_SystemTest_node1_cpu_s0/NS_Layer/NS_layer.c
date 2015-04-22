/**************************************************************
 * FILE:           NS_layer.c
 *
 * DESCRIPTION:
 * The Network and Service Layer (NS-Layer) is used by MCAPI
 * to communicate with other MCAPI nodes using existing
 * physical communication channels between the MCAPI nodes.
 *
 * It provides three different network services. Each service
 * has a different packet format. NS-layer takes the multiplexer
 * and demultiplexer job to send and receive the service
 * specific packets via the PI-layer.
 *
 * Supported services are:
 *   - 'getRemoteEndpoint' service implemented with the functions:
 *     	    NS_getRemoteEndpoint_request() and
 *     	    NS_getRemoteEndpoint_response())
 *   - 'endpointChannelIsopen' service implemented with the functions:
 *          NS_endpointChannelIsopen_request() and
 *          NS_endpointChannelIsopen_response()
 *   - 'sendDataToRemote' service implemented with the functions:
 *          NS_sendDataToRemote_request() and
 *          NS_sendDataToRemote_indication()
 *
 * Furthermore NS-layer takes the routing job, which means that
 * the logical network address is translated to the physical
 * address of the interface we have to use in order to reach the
 * destination node. Logical network address is given by the
 * MCAPI domain and node ID numbers, physical network address
 * depends on the kind of physical communication channel which
 * has to be used. E. g. if this communication channel is a
 * TCP based channel, than the physical address would be given
 * by TCP channel's socket descriptor.
 *
 * EDITION HISTORY:
 * 2013-11-20: getting started - ms (M. Strahnen)
 * 2013-12-05: further work was done - ms
 * 2014-01-27: synchronization with uC/OS-II based code - ms
 * 2014-04-19: introduction of NS layer debugging flag
 *             NS_DEBUG_ON                              - ms
 * 2014-04-21: pthread_kill() replaced by pthread_cancel() - ms
 * 2014-05-01: Adaptation to SoCkit board (use of FifoDriver
 *             disabled) - ms
 * 2014-06-04: Socket communication support implemented - ms
 * 2014-06-05: If remote node has returned MCAPI_FALSE,
 *             service functions will return NS_ERROR - ms
 * 2014-06-11: TCP-Socket length encoded framing
 *             implemented - ms
 * 2014-07-07: Version 0.4.2
 *             NS layer packet format extended. For confirmed
 *             services request packets will now contain
 *             source domainID and source nodeID in order to
 *             better support NS layer routing. - ms
 * 2014-07-29: Changes in NS_sendDataToRemote_request(),
 *             scalar value now passed in 'normal' data
 *             field - ms
 * 2014-08-05: v042 changes and ifdef adaptation to
 *             uC/OS-II - ms
 * 2014-08-11: mcapi_env.h included - ms
 * 2014-08-12: Defines FIFO and SOCK included - ms
 * 2014-08-13: Second FIFO receive task enabled - ms
 * 2014-09-30: Linux/Sock configuration: debugging:
 *             NS_receiveTask1 only in case of FIFO
 *             enabled - ms
 * 2014-10-14: In NS_sendDataToRemote_request(): copy of
 *             payload data only for linux version - ms
 *************************************************************/

#include "../MCAPI_Top/mcapi_env.h"

#ifdef LINUX
// Linux specific includes
	#include <semaphore.h>
	#include <pthread.h>
	#include <signal.h>
	#include <fcntl.h>	// O_RDWR
	#include <unistd.h>	// sleep()
	#include "../MCAPI_Transport/mcapi_trans_nios.h"
	#include "../MCAPI_Transport/mcapi_trans.h"
	#include "../PH_FifoDriver_UCOSII/PH_layer.h"
	#include "../PH_TCPSock_Linux/PH_TCPSock_layer.h"
#endif

#ifdef UCOSII
	#include "../MCAPI_Transport/mcapi_trans_nios.h"
	#include "../MCAPI_Transport/mcapi_trans.h"
	#include "../PH_FifoDriver_UCOSII/PH_layer.h"
	#include "includes.h"
#endif

#include "globals.h"
#include "mapping.h"
#include "../UTILS/util_packet_handling.h"
#include <assert.h>

//#define NS_DEBUG_ON			// NS layer debugging flag

#define TIM_DEL1_MS 10	// time delay in milli seconds we have
			// to wait for an MCAPI service, e.g.
			// if remote is not up

// global variables
extern uint32_t	my_domain_id;	// this node's domain
extern uint32_t	my_node_id;		// this node's ID

/************************************************************/
// Threads, etc

#ifdef LINUX
	pthread_t thread0, thread1; // receive threads
	int filedescriptor[2];
#endif

#define BASE_ADDRESS_FIFO_0 0x04000000
#define BASE_ADDRESS_FIFO_1 0x04000400

/************************************************************/
// ID numbers for NS service related packets:
#define	GET_REMOTE_ENDPOINT_REQUEST			10
#define GET_REMOTE_ENDPOINT_RESPONSE		11
#define	ENDPOINT_CHANNEL_ISOPEN_REQUEST		20
#define	ENDPOINT_CHANNEL_ISOPEN_RESPONSE	21
#define SEND_DATA_TO_REMOTE_REQUEST			30

// NS layer may return following error/status codes:
#define NS_OK				 		  0	// no error, everything works fine
#define	NS_ERROR		 			 -1	// an error occured
#define NS_NO_FREE_SERVICEREQUEST_ID -2	// no free ID available
#define	NS_NO_FREE_CALLID			 -3	// no free callID available

/************************************************************/
// Following data structure describes a NS_layer protocol
// data unit (NS_pduA). Have care: payload is a pointer,
// i.e. memory allocation for payload area is NOT done
// here
typedef struct {
	uint32_t	srcDomain;
	uint32_t	srcNode;
    uint16_t	NS_serviceID;
    uint16_t	callID;
    uint32_t	payloadLength;
    uint8_t		*payload;
} NS_pduA;

// Following data structure describes a NS_layer protocal
// data unit (NS_pduB). Have care: payload is declared as
// an array, i.e. memory allocation is done HERE. This
// pdu type is only used for response messages. Maximum
// payload size of a response is limited to
// MAX_RESP_PAYLOAD_SIZE
typedef struct {
	uint32_t	srcDomain;
	uint32_t	srcNode;
    uint16_t	NS_serviceID;
    uint16_t	callID;
    uint32_t	payloadLength;
    uint8_t		payload[MAX_NS_RESP_PAYLOAD_SIZE];
} NS_pduB;

/************************************************************/
// wServiceDescriptor describes a waiting service request
typedef struct sFuncCall {
	uint16_t NS_serviceID;
	uint16_t callID;
	uint8_t	inuse;	// inuse = 1: structure is in use
					// inuse = 0: structure is free
#ifdef UCOSII
	OS_EVENT *semSync;	// semaphore by which call specific waiting function
					// is informed that response has been received
#endif
#ifdef LINUX
	sem_t *semSync;	// semaphore by which call specific waiting function
					// is informed that response has been received
#endif
	NS_pduB syncResponse;	// response message description
} wServiceDescriptor;

#define MAX_NS_SERVICE_REQUESTS	8	// maximum number of waiting
									// service requests
#ifdef UCOSII
	OS_EVENT *sem_serviceRequest[MAX_NS_SERVICE_REQUESTS];	// array of
					// semaphores which are used to wait for outstanding
					// service requests
#endif

#ifdef LINUX
	sem_t sem_serviceRequest[MAX_NS_SERVICE_REQUESTS];	// array of
			// semaphores which are used to wait for outstanding
			// service requests
#endif

// Array of structures which could be used to describe
// a waiting service request. At maximum we could wait
// for MAX_NS_SERVICE_REQUESTS service requests.
wServiceDescriptor serviceRequest[MAX_NS_SERVICE_REQUESTS];

/************************************************************/
// unique callID number:
// One node could have several outstanding requests. To
// identify each outstanding request we use the so called
// callID parameter. Before sending a request we have to
// acquire a unique callID number. The instance responding
// to a request will include callID in it's response.
#define MAX_CALLIDS 16		// maximum number of outstanding calls
#define MAX_CALLID_NUM 32
uint16_t nextCallID = 0;	// index of next free callID entry
int16_t  callIDs[MAX_CALLIDS];	// array of callIDs
uint16_t callIDcount = 0;	// array entries in use

/************************************************************/
// Mutex reentMutex is used to ensure reentrancy of some
// functions
#ifdef UCOSII
	OS_EVENT	*reentMutex;
	#define	REENT_MUTEX_PRIO	4
#endif

#ifdef LINUX
	pthread_mutex_t reentMutex;
#endif

/************************************************************/
// necessary function prototypes
int16_t	NS_getCallID(void);
void	NS_releaseCallID(int16_t id);
int16_t	NS_get_serviceRequestID();
void	NS_unlock_waiting_request(NS_pduA *packet);
void 	NS_layer_receive(PH_pdu *data);

/**************************************************************
 * FUNCTION: getIndex
 *
 * DESCRIPTION:
 * the function helps to find out the right index for filedescriptor
 *
 * INPUT PARAMETERS:
 * - bridge_base
 *
 * RETURN VALUE:
 * - integer value
 *************************************************************/
int getIndex(uint32_t bridge_base)
{
	switch(bridge_base) {
		case BASE_ADDRESS_FIFO_0:
			return(0);
			break;
		case BASE_ADDRESS_FIFO_1:
			return(1);
			break;
		default:
			break;
	}

	return(-1);
}

/**************************************************************
 * FUNCTION: NS_get_serviceRequestID()
 *
 * DESCRIPTION:
 * Utility function which checks if there is a free element in
 * serviceRequest[] array available. If yes, index number of element
 * found will be returned. If not, error code -1 will be
 * returned.
 *
 * INPUT PARAMETERS: -
 *
 * RETURN VALUE:
 * - index number of free array entry or one of the
 *   following error codes:
 *   - NS_NO_FREE_SERVICEREQUEST_ID
 *   - NS_ERROR
 *************************************************************/
int16_t NS_get_serviceRequestID(void) {
	int16_t i;
	uint8_t err;

	// Take reentMutex to ensure reentrancy
#ifdef UCOSII
	OSMutexPend(reentMutex, 0, &err);
	if(err != OS_NO_ERR) {
		printf("NS_get_serviceRequestID: reentMutex-pend error\n");
		return(NS_ERROR);
	}
#endif

#ifdef LINUX
	err = pthread_mutex_lock(&reentMutex);
	if(err != 0) {
		printf("NS_get_serviceRequestID: reentMutex-pend error\n");
		return(NS_ERROR);
	}
#endif

	// search for next free element
	for(i = 0; i < MAX_NS_SERVICE_REQUESTS; i++) {
		if(serviceRequest[i].inuse == 0) {
			serviceRequest[i].inuse = 1;	// lock element
			// Release reentMutex
#ifdef UCOSII
			if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
				printf("NS_get_serviceRequestID: reentMutex-post error\n");
				return(NS_ERROR);
			}
#endif

#ifdef LINUX
			if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
				printf("NS_get_serviceRequestID: reentMutex-post error\n");
				return(NS_ERROR);
			}
#endif
			return(i);
		}
	}

	// Release reentMutex
#ifdef UCOSII
	if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
		printf("NS_get_serviceRequestID: reentMutex-post error\n");
		return(NS_ERROR);
	}
#endif

#ifdef LINUX
	if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
		printf("NS_get_serviceRequestID: reentMutex-post error\n");
		return(NS_ERROR);
	}
#endif

	return(NS_NO_FREE_SERVICEREQUEST_ID);	// no free ID
}											// available

/*************************************************************
 * FUNCTION: NS_getCallID()
 *
 * DESCRIPTION:
 * The purpose of callID is to clearly identify any
 * outstanding request. Each response message contains the
 * callID of the requesting unit, and by that, the waiting
 * request clearly could be identified.
 *
 * RETURN VALUE:
 * - 16-bit unique callID number
 * - one of the following error code:
 *   - NS_NO_FREE_CALLID
 *************************************************************/
int16_t NS_getCallID(void)
{
	uint8_t free = 1;
	int i = 0;
	uint8_t err;

	// Take reentMutex to ensure reentrancy
#ifdef UCOSII
	OSMutexPend(reentMutex, 0, &err);
	if(err != OS_NO_ERR) {
		printf("NS_getCallID: reentMutex-pend error\n");
		return(NS_ERROR);
	}
#endif

#ifdef LINUX
	err = pthread_mutex_lock(&reentMutex);
	if(err != 0) {
		printf("NS_getCallID: reentMutex-pend error\n");
		return(NS_ERROR);
	}
#endif

	// nextCallID contains next regular callID number.
	// But we have to check if this number is still in use.
	// If true we have to check the next nextCallID.
	do {	// generate new free call ID number
		free = 1;

		if (nextCallID > MAX_CALLID_NUM) {	// callIDs should have
			nextCallID = 0;			// value in range
		}					// 0 to MAX_CALLID_NUM

		for (i = 0; i < callIDcount; ++i) {
			if (callIDs[i] == nextCallID) {
				free = 0;
			}
		}
		++nextCallID;
	} while(!free);

	// check if there is enough storage capacity for the new call ID
	if(callIDcount < MAX_CALLIDS) {
		callIDs[callIDcount] = nextCallID - 1;	// store call ID number
		++callIDcount;

		// Release reentMutex
#ifdef UCOSII
		if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
			printf("NS_getCallID: reentMutex-post error\n");
			return(NS_ERROR);
		}
#endif
#ifdef LINUX
		if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
			printf("NS_getCallID: reentMutex-post error\n");
			return(NS_ERROR);
		}
#endif
		return(nextCallID -1);	// return callID
	}
	else {
		--nextCallID;	// because the generated call id will not
				// be used now

		// Release reentMutex
#ifdef UCOSII
		if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
			printf("NS_getCallID: reentMutex-post error\n");
			return(NS_ERROR);
		}
#endif
#ifdef LINUX
		if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
			printf("NS_getCallID: reentMutex-post error\n");
			return(NS_ERROR);
		}
#endif
		return((int16_t) NS_NO_FREE_CALLID);	// return error code
	}
}

/*************************************************************
 * FUNCTION: NS_releaseCallID()
 *
 * DESCRIPTION:
 * Array callIDs[] stores the callID numbers of any outstanding
 * requests. As soon as a request has been handled, we have
 * to mark request's callID number as free. This is done in
 * this function:
 *
 * INPUT PARAMETERS:
 * - id: callID number we have to release
 *
 * RETURN VALUE:
 * - none
 *************************************************************/
void NS_releaseCallID(int16_t id) {
	int16_t callID = (0x3FFF & id);	// clear most significant two bits
					// which represent typeID parameter
	int	i;
	int	k;
	uint8_t err;

	// Take reentMutex to ensure reentrancy
#ifdef UCOSII
	OSMutexPend(reentMutex, 0, &err);
	if(err != OS_NO_ERR) {
		printf("NS_releaseCallID: reentMutex-pend error\n");
		return;
	}
#endif
#ifdef LINUX
	err = pthread_mutex_lock(&reentMutex);
	if(err != 0) {
		printf("NS_releaseCallID: reentMutex-pend error\n");
		return;
	}
#endif

	for (i = 0; i < callIDcount; ++i) {
		if (callID == callIDs[i]) {
			for(k=i; k < callIDcount - 1; ++k) { // rearrange array
				callIDs[k] = callIDs[k+1];
			}
		--callIDcount;

		// Release reentMutex
#ifdef UCOSII
		if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
			printf("NS_releaseCallID: reentMutex-post error\n");
			return;
		}
#endif
#ifdef LINUX
		if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
			printf("NS_releaseCallID: reentMutex-post error\n");
			return;
		}
#endif

		return;
		}
	}

	// Release reentMutex
#ifdef UCOSII
	if((err = OSMutexPost(reentMutex)) != OS_NO_ERR) {
		printf("NS_releaseCallID: reentMutex-post error\n");
		return;
	}
#endif
#ifdef LINUX
	if((err = pthread_mutex_unlock(&reentMutex)) != 0) {
		printf("NS_releaseCallID: reentMutex-post error\n");
		return;
	}
#endif
}

// -------------------------------------- Receive-Threads --------------------------------------
#ifdef LINUX	// receive threads are only required for Linux
/*************************************************************
 * TASK: NS_receiveThread0()
 *
 * DESCRIPTION:
 * Have care! Description only concern FIFO com. channel!!!
 * Each FIFO channel has its own FIFO driver, which
 * supports a blocking read operation. Driver's read function
 * will return if physical interface layer (PI-layer) has
 * received a new packet. In order not to block the entire
 * NS layer software we have to spend a receive thread for
 * each FIFO channel.
 *
 * INPUT PARAMETERS: -
 *
 * RETURN VALUE: -
 *************************************************************/
/* receive thread for FIFO channel 0 */
void NS_receiveTask0(void *arg)
{
	int *filep = (int*) arg;
	uint32_t length;
	uint32_t bufferSize = MAX_NS_PACK_SIZE;
	uint8_t staticBuffer[bufferSize];

	PH_pdu data;
	data.bridge_base = BASE_ADDRESS_FIFO_0;	// We have to indicate
						// the FIFO channel because in some case we
						// have to send back a response

	// allow the thread to be cancelled asychonously with pthread_cancel()
	if(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0)
               printf("NS_receiveTask0: error with pthread_setcancelstate()\n");
	if(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL) != 0)
               printf("NS_receiveTask0: error with pthread_setcancelstate()\n");

	while(1) {
#ifdef FIFO
		length = read(*filep, staticBuffer, bufferSize);
#endif
#ifdef SOCK
		if((length = PH_TCPSock_recv(staticBuffer, bufferSize)) <= 0) {
			printf("NS_receiveTask0: error in socket read/recv function\n"); fflush(stdout);
		}
#endif
		data.length = length;
		data.data = staticBuffer;

#ifdef NS_DEBUG_ON
		printf("NS_receiveTask0:  Packet with %d bytes received\n", data.length); fflush(stdout);
	int ii = 0;
	printf("NS_receiveTask0: ");
	for(ii = 0; ((ii < data.length) && (ii < 30)); ii++)
		printf("0x%x, ", staticBuffer[ii]);
	printf("\n");
#endif
		NS_layer_receive(&data);	// Analyse received packet and do
									// the requested operations
	}

	pthread_exit(NULL);
}

/**************************************************************
 * TASK: NS_receiveTask1()
 *
 * receive thread for FIFO channel 1
 * HAVE CARE!
 * Because actually only 1 SOCK-channel is supported, the 
 * therefore required code is disabled in this task. It is 
 * enabled only in NS_receiveTask0() code.
 *************************************************************/
void NS_receiveTask1(void *arg)
{
	int *filep = (int*) arg;
	uint32_t length;
	uint32_t bufferSize = MAX_NS_PACK_SIZE;
	uint8_t staticBuffer[bufferSize];

	PH_pdu data;
	data.bridge_base = BASE_ADDRESS_FIFO_1;	// We have to indicate
						// the FIFO channel because in some case we
						// have to send back a response

	// allow the thread to be cancelled asychonously with pthread_cancel()
	if(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0)
               printf("NS_receiveTask1: error with pthread_setcancelstate()\n");
	if(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,NULL) != 0)
               printf("NS_receiveTask1: error with pthread_setcancelstate()\n");

	while(1) {
#ifdef FIFO
		length = read(*filep, staticBuffer, bufferSize);
#endif
#ifdef SOCK
//		if((length = PH_TCPSock_recv(staticBuffer, bufferSize)) <= 0) {
//			printf("NS_receiveTask0: error in socket read/recv function\n"); fflush(stdout);
//		}
#endif
		data.length = length;
		data.data = staticBuffer;

#ifdef NS_DEBUG_ON
		printf("NS_receiveTask1:  Packet with %d bytes received\n", data.length); fflush(stdout);
	int ii = 0;
	printf("NS_receiveTask1: ");
	for(ii = 0; ((ii < data.length) && (ii < 30)); ii++)
		printf("0x%x, ", staticBuffer[ii]);
	printf("\n");
#endif
		NS_layer_receive(&data);	// Analyse received packet and do
									// the requested operations
	}

	pthread_exit(NULL);
}

#endif // LINUX

/**************************************************************
 * function: NS_layer_exit()
 *
 * Description:
 * Destroy receive threads, close communication devices and
 * destroy mutexes and semaphores.
 *
 * Input parameters:
 *  - none
 *
 * Return value:
 * Returns error/status code. Following codes might be returned:
 * - NS_OK      everything works fine
 * - NS_ERROR   error occured
 *************************************************************/
int NS_layer_exit()
{
	int	err = NS_OK;

#ifdef LINUX
	if(pthread_cancel(thread0) != 0) {
		printf("NS_layer_exit: can't cancel receive thread0\n");
		err = NS_ERROR;
	}

#ifdef FIFO	// actually SOCK option supports only one receive thread
	if(pthread_cancel(thread1) != 0) {
		printf("NS_layer_exit: can't cancel receive thread1\n");
		err = NS_ERROR;
	}
#endif

//	if(pthread_kill(thread1, SIGTERM) != 0) {
//	if(pthread_cancel(thread1) != 0) {
//		printf("NS_layer_exit: can't kill receive thread1\n");
//		err = NS_ERROR;
//	}

#ifdef FIFO
	if(close(filedescriptor[0]) != 0) {
		printf("NS_layer_exit: can't close communication device!\n");
		err = NS_ERROR;
	}

	if(close(filedescriptor[1]) != 0) {
		printf("NS_layer_exit: can't close communication device!\n");
		err = NS_ERROR;
	}
#endif // FIFO

#ifdef SOCK
	// deinitialize TCP socket based communication
	if(PH_TCPSock_deinit(my_node_id) == PH_TCPSock_ERROR) return(NS_ERROR);

	if(pthread_mutex_destroy(&reentMutex) != 0) {
		printf("NS_layer_exit: can't destroy mutex!\n");
		err = NS_ERROR;
	}
#endif // SOCK

#endif // LINUX

	return err;
}

/*************************************************************
 * function: NS_init()
 *
 * Description:
 * Initialization of NS layer and next lower layer components.
 *
 * Input parameters:
 *  - local node ID
 *
 * Return value:
 * Returns error/status code. Following codes might be returned:
 * - NS_OK      everything works fine
 * - NS_ERROR   error occured
 *************************************************************/
uint8_t NS_init(uint32_t domain_id, uint32_t node_id)
{
	uint8_t	err;

	my_domain_id = domain_id;
	my_node_id   = node_id;

	// Initialize serviceRequest[] structures and
	// get semaphores for serviceRequest[] structures
	int i;
	for(i = 0; i < MAX_NS_SERVICE_REQUESTS; i++) {
		serviceRequest[i].NS_serviceID = -1;
		serviceRequest[i].callID = -1;
		serviceRequest[i].inuse = 0;
		serviceRequest[i].semSync = NULL;
		serviceRequest[i].syncResponse.callID = -1;
		serviceRequest[i].syncResponse.NS_serviceID = -1;
		serviceRequest[i].syncResponse.payloadLength = 0;
#ifdef UCOSII
		sem_serviceRequest[i] = OSSemCreate(0);
		if(sem_serviceRequest[i] == NULL) {
			printf("NS_init: error when creating semaphore\n");
			return(NS_ERROR);
		}
		serviceRequest[i].semSync = sem_serviceRequest[i];
#endif
#ifdef LINUX
		err = sem_init(&sem_serviceRequest[i],0,0);
		if(err != 0){
			printf("NS_init: error when creating semaphore\n");
			return(NS_ERROR);
		}
		serviceRequest[i].semSync = &sem_serviceRequest[i];
#endif
	}

#ifdef NS_DEBUG_ON
	printf("NS_init: serviceRequest structures initialized\n");
	fflush(stdout);
#endif

	// reentMutex is used by several functions to ensure their
	// reentrancy
#ifdef UCOSII
	reentMutex = OSMutexCreate(REENT_MUTEX_PRIO, &err);
	if(err != OS_NO_ERR) {
		printf("NS_init: error when creating reentMutex\n");
		return(NS_ERROR);
	}
#endif
#ifdef LINUX
	err = pthread_mutex_init(&reentMutex, NULL);
	if(err != 0) {
		printf("NS_init: error when creating reentMutex\n");
		return(NS_ERROR);
	}
#endif
#ifdef NS_DEBUG_ON
	printf("NS_init: reentMutex has been created\n");
	fflush(stdout);
#endif

	// initialize lower layer software
#ifdef UCOSII
	if(PH_init(my_node_id) != PH_OK) {	// Initialize next (lower) layer
		printf("NS_init: error on PH_layer initialization\n");
		return(NS_ERROR);
	}
#endif

#ifdef LINUX
#ifdef FIFO
	// Open driver device which handles FIFO0 communication channel
	if((filedescriptor[0] = open("/dev/FifoDriver0", O_RDWR)) == -1) {
		printf("NS_init: error when opening device /dev/FifoDriver0\n");
		return(NS_ERROR);
	}

	// Open driver device which handles FIFO1 communication channel
	if((filedescriptor[1] = open("/dev/FifoDriver1", O_RDWR)) == -1) {
		printf("NS_init: error when opening device /dev/FifoDriver1\n");
		return(NS_ERROR);
	}
#endif // FIFO

#ifdef SOCK
	// initialize TCP-based socket layer
	if(PH_TCPSock_init(my_node_id) != PH_TCPSock_OK)	return(NS_ERROR);
#endif // SOCK
#ifdef NS_DEBUG_ON
	printf("NS_init: PH layer software has been initialized\n");
	fflush(stdout);
#endif

	// Create one receive thread per FIFO communication channel
	err = pthread_create(&thread0, NULL, (void *) NS_receiveTask0, &filedescriptor[0]);
	if(err != 0){
		printf("NS_init: Pthread_create failed");
		return(NS_ERROR);
	}

#ifdef FIFO	// actually SOCK option supports only one receive thread
	err = pthread_create(&thread1, NULL, (void *) NS_receiveTask1, &filedescriptor[1]);
	if(err != 0){
		printf("NS_init: Pthread_create failed");
		return(NS_ERROR);
	}
#endif

#ifdef NS_DEBUG_ON
	printf("NS_init: receive threads have been created\n");
	fflush(stdout);
#endif	// NS_DEBUG_ON
#endif // LINUX
	return NS_OK;
}

/*************************************************************
 * FUNCTION: NS_getRemoteEndpoint_request()
 *
 * DESCRIPTION:
 * With this function upper layer software, in our case
 * MCAPI_trans, requests the service 'getRemoteEndpoint'.
 * Function gets the remote endpoint ID of a remote endpoint
 * given by it's domain, node and port IDs.
 *
 * INPUT PARAMETERS:
 *  endpoint  - the endpoint handle to be filled in
 *  domain_id - the domain id
 *  node_num  - the node id
 *  port_num  - the port id
 *
 * RETURN VALUE:
 * Returns error/status code. Following codes might be returned:
 * - NS_OK      everything works fine
 * - NS_ERROR   error occured (Have care! If remote node has
 *              returned MCAPI_FALSE, we will return NS_ERROR)
 *************************************************************/
uint8_t NS_getRemoteEndpoint_request(uint32_t *endpoint, uint32_t domain_id, uint32_t node_num, uint32_t port_num)
{
	uint8_t  err;
	uint16_t NS_serviceID = GET_REMOTE_ENDPOINT_REQUEST;
	uint32_t bufferSize = 24; 	// request message size
	int16_t	 serviceRequestID;
	uint32_t bridge_base;		// Avalon slave address of dest. FIFO
	int index;      			// ???

	//check if destination node parameters are valid
	if(!nios_node_mapping_db[my_node_id].destination_nodes[node_num].valid) {
		return(NS_ERROR);
	}

	// check if required buffer size does not exceed maximum buffer size
	assert(bufferSize <= MAX_NS_PACK_SIZE);

	// get unique callID number
	int16_t callID = NS_getCallID();
	if(callID == NS_NO_FREE_CALLID) {
		printf("NS_getRemoteEndpoint_request: no free call ID available\n");
		return(NS_ERROR);
	}

	// alignment of msg[] buffer forced because later 4 byte load
	// and store instructions may be used to copy buffer
	uint8_t msg[bufferSize] __attribute__ ((aligned (4)));

#ifdef NS_DEBUG_ON
	printf("NS_getRemoteEndpoint_request: \n"); fflush(stdout);
	printf("      - get endpoint ID of: domain/node/port = %d, %d, %d\n", domain_id, node_num, port_num);
	fflush(stdout);
#endif

	// Prepare service request packet
	pack_add_u32(msg,  0, my_domain_id);	// Header: source domain ID
	pack_add_u32(msg,  4, my_node_id);		// Header: source node ID
	pack_add_u16(msg,  8, NS_serviceID);	// Header: Service-ID
	pack_add_u16(msg, 10, callID);			// Header: callID
	pack_add_u32(msg, 12, domain_id);		// Payload: endpoint's domain number
	pack_add_u32(msg, 16, node_num);		// Payload: endpoint's node number
	pack_add_u32(msg, 20, port_num);		// Payload: endpoint's port number

	// Get a service request specification entry where we could
	// specify that we are waiting for a response.
	serviceRequestID = NS_get_serviceRequestID();
	if(serviceRequestID == -1) {
		printf("NS_getRemoteEndpoint_request: no free serviceRequest-structure available\n");
		return(NS_ERROR);
	}

	// Specify our soon waiting request
	serviceRequest[serviceRequestID].NS_serviceID = NS_serviceID;
	serviceRequest[serviceRequestID].callID = (((uint16_t)0x3FFF) & callID);

	// Find out the route, i.e. the base address of the FIFO
	// bridge or the index of the Linux driver module which
	// is connected to the remote endpoint and send request
	bridge_base = nios_node_mapping_db[my_node_id].destination_nodes[node_num].base;
	// find out the right index for filedescriptor
	index = getIndex(bridge_base);
//	index = nios_node_mapping_db[my_node_id].destination_nodes[node_num].index;
	//printf("index: %i ", index);
#ifdef UCOSII
	PH_send_request(bridge_base, (uint32_t *)msg, (uint32_t) bufferSize, (uint32_t *)NULL, (uint32_t) 0);

	// Wait for the answer from the remote side
	OSSemPend(serviceRequest[serviceRequestID].semSync, 0, &err);
	if(err != OS_NO_ERR) {
		printf("NS_getRemoteEndpoint_request: OSSemPend() failed\n");
		return(NS_ERROR);
	}
#endif
#ifdef LINUX
#ifdef FIFO
	if(write(filedescriptor[index], msg, bufferSize) != bufferSize) {
		printf("NS_getRemoteEndpoint_request: Error on writing to com. device!\n");
		return(NS_ERROR);
	}
#endif // FIFO

#ifdef SOCK
	if(PH_TCPSock_send(msg, bufferSize) != bufferSize) {
		printf("NS_getRemoteEndpoint_request: Error on writing to com. device!\n");
		return(NS_ERROR);
	}
#endif // SOCK

	// Wait for the answer from the remote side
	err = sem_wait(serviceRequest[serviceRequestID].semSync);
	if(err != 0) {
		printf("NS_getRemoteEndpoint_request: sem_wait() failed\n");
		return(NS_ERROR);
	}
#endif

	// get status of remote MCAPI call and remote endpoint ID
	uint8_t remote_MCAPI_status = pack_get_u8(serviceRequest[serviceRequestID].syncResponse.payload,0);
	*endpoint = pack_get_u32(serviceRequest[serviceRequestID].syncResponse.payload,1);

#ifdef NS_DEBUG_ON
	printf("NS_getRemoteEndpoint_request: \n"); fflush(stdout);
	printf("      - received endpoint ID for: domain/node/port = %d, %d, %d\n", domain_id, node_num, port_num);
	printf("      - is: 0x%x\n", *endpoint);
	printf("      - return status is: %d\n", remote_MCAPI_status);
	fflush(stdout);
#endif

	serviceRequest[serviceRequestID].inuse = 0;	// free serviceRequest[] element
	NS_releaseCallID(callID);

	if(remote_MCAPI_status) return(NS_OK);		// remote node has delivered MCAPI_TRUE
	else					return(NS_ERROR);	// remote node has delivered MCAPI_FALSE
}

/*************************************************************
 * FUNCTION: NS_get_remote_endpoint_response()
 *
 * DESCRIPTION:
 * If a remote instance has called service 'getRemoteEndpoint'
 * by calling NC_getRemoteEndpoint_request() this will lead
 * to a call of this function. Function will try to get the
 * requested endpoint and will respond the endpoint handle to
 * the requesting node.
 *
 * INPUT PARAMETERS:
 *  bridge_base  - base address of the FIFO we have to use
 *                 to send the response
 *  packet       - pointer to the original request message
 *
 * RETURN VALUE: -
 *************************************************************/
void NS_getRemoteEndpoint_response(uint32_t bridge_base, NS_pduA *packet)
{
	uint32_t endpoint = 0;

	// extract parameters send by client
	uint32_t domain_id = pack_get_u32(packet->payload, 0);
	uint32_t node_num = pack_get_u32(packet->payload, 4);
	uint32_t port_num = pack_get_u32(packet->payload, 8);

	// If remote node is up before we are up, we have to wait
	// until MCAPI_Transport layer is fully initialized.
	while(mcapi_trans_initialized() != MCAPI_TRUE)
		usleep(TIM_DEL1_MS * 1000);

	uint8_t retPtr = mcapi_trans_endpoint_get_(&endpoint, domain_id, node_num, port_num);

#ifdef NS_DEBUG_ON
	printf("NS_getRemoteEndpoint_response: \n"); fflush(stdout);
	printf("      - remote endpoint ID of: domain/node/port = %d, %d, %d\n", domain_id, node_num, port_num);
	printf("                               is 0x%x\n", endpoint);
	fflush(stdout);
#endif

	// prepare return message to client
	// alignment of msg[] buffer forced because later 4 byte load
	// and store instructions may be used to copy buffer
	uint8_t msg[17] __attribute__ ((aligned (4)));

	pack_add_u32(msg,  0, my_domain_id);							// Header: source domain ID
	pack_add_u32(msg,  4, my_node_id);								// Header: source node ID
	pack_add_u16(msg,  8, (uint16_t) GET_REMOTE_ENDPOINT_RESPONSE);	// Header: Service-ID
	pack_add_u16(msg, 10, packet->callID);							// Header: callID
	pack_add_u8(msg, 12, retPtr);									// Payload: local MCAPI return status
	pack_add_u32(msg,13, endpoint);									// Payload: endpoint handle

	// Send response message
#ifdef UCOSII
	 PH_send_request(bridge_base, (uint32_t *)msg, (uint32_t) 17, (uint32_t *)NULL, (uint32_t) 0);
#endif
#ifdef LINUX
	int index = getIndex(bridge_base);

#ifdef FIFO
	if(write(filedescriptor[index], msg, 17) != 17)
		printf("NS_getRemoteEndpoint_response: error when sending response\n");
#endif // FIFO

#ifdef SOCK
	if(PH_TCPSock_send(msg, 17) != 17)
		printf("NS_getRemoteEndpoint_response: error when sending response\n");
#endif // SOCK

#endif // LINUX
}

/*************************************************************
 * FUNCTION: NS_endpointChannelIsopen_request()
 *
 * DESCRIPTION:
 * With this function upper layer software, in our case
 * MCAPI_trans, requests the service 'endpointChannelIsopen'.
 * Function will send a request to the node which is
 * expected to possess the specified endpoint.
 *
 * INPUT PARAMETERS:
 *  endpoint  - endpoint belonging to the channel
 *
 * RETURN VALUE:
 * Returns error/status code. Following codes might be returned:
 * - NS_OK      remote node has delivered MCAPI_TRUE
 * - NS_ERROR   remote node has delivered MCAPI_FALSE
 *************************************************************/
uint8_t NS_endpointChannelIsopen_request(uint32_t endpoint)
{
	uint16_t rd,rn,re;
	uint8_t err;
	uint32_t bridge_base;
	uint16_t NS_serviceID = ENDPOINT_CHANNEL_ISOPEN_REQUEST;
	uint32_t bufferSize = 16; // buffer size
	int16_t	serviceRequestID;

	mcapi_trans_decode_handle(endpoint,&rd,&rn,&re);
	// check if destination node parameters are valid
	if(!nios_node_mapping_db[my_node_id].destination_nodes[rn].valid) {
		return(NS_ERROR);
	}

	// check if required buffer size does not exceed maximum buffer size
	assert(bufferSize <= MAX_NS_PACK_SIZE);

	// get unique callID number
	int16_t callID = NS_getCallID();
	if(callID == NS_NO_FREE_CALLID) {
		printf("NS_endpointChannelIsopen_request: error NS_NO_FREE_CALLID occured\n");
		return(NS_ERROR);
	}

	// alignment of msg[] buffer forced because later 4 byte load
	// and store instructions may be used to copy buffer
	uint8_t msg[bufferSize] __attribute__ ((aligned (4)));

	// Now request packet will be built
	pack_add_u32(msg,  0, my_domain_id);	// Header: source domain ID
	pack_add_u32(msg,  4, my_node_id);		// Header: source node ID
	pack_add_u16(msg,  8, NS_serviceID);	// Header: Service-ID
	pack_add_u16(msg, 10, callID);			// Header: callID
	pack_add_u32(msg, 12, endpoint);		// Payload: endpoint

	// Function we have to wait for will be described
	// with one of the structures in serviceRequest[].
	// First get index number of a free serviceRequest[] structure
	serviceRequestID = NS_get_serviceRequestID();
	if(serviceRequestID == -1) {
		printf("NS_endpointChannelIsopen_request: no serviceRequestID[] structure available\n");
		return(NS_ERROR);
		}
	// Specify our soon waiting request
	serviceRequest[serviceRequestID].NS_serviceID = NS_serviceID;
	serviceRequest[serviceRequestID].callID = (((uint16_t)0x3FFF) & callID);

	// Find out the route, i.e. the base address of the FIFO bridge
	bridge_base = nios_node_mapping_db[my_node_id].destination_nodes[rn].base;

	// Send packet using PI layer service
#ifdef UCOSII
	PH_send_request(bridge_base, (uint32_t *)msg, (uint32_t) bufferSize, (uint32_t *)NULL, (uint32_t) 0);

	// Wait for answer from remote node
	OSSemPend(serviceRequest[serviceRequestID].semSync, 0, &err);
	if(err != OS_NO_ERR) {
		printf("NS_endpointChannelIsopen_request: OSSemPend() failed\n");
		return(NS_ERROR);
	}
#endif
#ifdef LINUX
	int index = getIndex(bridge_base);  //find out the right index for filedescriptor

#ifdef FIFO
	if(write(filedescriptor[index], msg, bufferSize) != bufferSize) {
		printf("NS_endpointChannelIsopen_request: error when sending data\n");
		return(NS_ERROR);
	}
#endif // FIFO

#ifdef SOCK
	if(PH_TCPSock_send(msg, bufferSize) != bufferSize) {
		printf("NS_endpointChannelIsopen_request: error when sending data\n");
		return(NS_ERROR);
	}
#endif // SOCK

	// Wait for answer from remote node
	err = sem_wait(serviceRequest[serviceRequestID].semSync);
	if(err != 0) {
		printf("NS_endpointChannelIsopen_request: error in sem_wait()\n");
		return(NS_ERROR);
	}
#endif // LINUX

	// Get payload part of slave's answer
	uint8_t remote_MCAPI_status = pack_get_u8(serviceRequest[serviceRequestID].syncResponse.payload,0);

	serviceRequest[serviceRequestID].inuse = 0;	// free serviceRequest[] element
	NS_releaseCallID(callID);

	if(remote_MCAPI_status) return(NS_OK);		// remote node has delivered MCAPI_TRUE
	else					return(NS_ERROR);	// remote node has delivered MCAPI_FALSE
}

/*************************************************************
 * FUNCTION: NS_endpointChannelIsopen_response()
 *
 * DESCRIPTION:
 * If a remote instance has called service 'endpointChannelIsopen'
 * by calling NS_endpointChannelIsopen_request() this will lead
 * to a call of this function. Function will check if there
 * is an opened channel with the specified endpoint and will
 * respond the status to the requesting node.
 *
 * INPUT PARAMETERS:
 *  bridge_base  - base address of the FIFO we have to use
 *                 to send the response
 *  packet       - pointer to the original request message
 *
 * RETURN VALUE: -
 *************************************************************/
void NS_endpointChannelIsopen_response(uint32_t bridge_base, NS_pduA *packet)
{
	// extract parameters send by remote node
	uint32_t endpoint = pack_get_u32(packet->payload, 0);

	// If remote node is up before we are up, we have to wait
	// until MCAPI_Transport layer is fully initialized.
	while(mcapi_trans_initialized() != MCAPI_TRUE)
		usleep(TIM_DEL1_MS * 1000);

	// check if there is an open channel
	uint8_t retPtr = mcapi_trans_endpoint_channel_isopen(endpoint);

	// alignment of msg[] buffer forced because later 4 byte load
	// and store instructions may be used to copy buffer
	uint8_t msg[13] __attribute__ ((aligned (4)));

	pack_add_u32(msg,  0, my_domain_id);							// Header: source domain ID
	pack_add_u32(msg,  4, my_node_id);								// Header: source node ID
	pack_add_u16(msg,  8, (uint16_t) ENDPOINT_CHANNEL_ISOPEN_RESPONSE);	// Header: Service-ID
	pack_add_u16(msg, 10, packet->callID);							// Header: callID
	pack_add_u8(msg, 12, retPtr);		      // Payload: local MCAPI return status

#ifdef NS_DEBUG_ON
	printf("NS_endpointChannelIsopen_response: response message:\n");
	printf("       -> domainID = %d, nodeID = %d\n", my_domain_id, my_node_id);
	printf("       -> serviceID = %d\n", (uint16_t) ENDPOINT_CHANNEL_ISOPEN_RESPONSE);
	printf("       -> callID = %d, retPtr = %d\n", packet->callID, retPtr);
	fflush(stdout);
#endif
	// send response message
#ifdef UCOSII
	PH_send_request(bridge_base, (uint32_t *)msg, (uint32_t) 13, (uint32_t *)NULL, (uint32_t) 0);
#endif
#ifdef LINUX
	int index = getIndex(bridge_base);

#ifdef FIFO
	if(write(filedescriptor[index], msg, 13) != 13)
		printf("NS_endpointChannelIsopen_response: Send Data failed");
#endif // FIFO

#ifdef SOCK
	if(PH_TCPSock_send(msg, 13) != 13)
		printf("NS_endpointChannelIsopen_response: Send Data failed");
#endif // SOCK

#endif // LINUX
}

/*************************************************************
 * FUNCTION: NS_sendDataToRemote_request()
 *
 * DESCRIPTION:
 * With this function upper layer software, in our case
 * MCAPI_trans, requests the service 'sendDataToRemote'.
 * Function will send data packet from source to destination
 * endpoint. Destination must be a remote endpoint.
 * Data packet could carry a message, a channel packet data
 * or a scalar value.
 *
 * INPUT PARAMETERS:
 *   - send_endpoint:    source endpoint
 *   - receive_endpoint: destination endpoint
 *   - buffer:           pointer to message location
 *   - buffer_size:      message size in bytes
 *
 * RETRUN VALUE:
 * - NS_OK      everything works fine
 * - NS_ERROR   error occured
 *************************************************************/
uint8_t NS_sendDataToRemote_request(uint32_t send_endpoint,
					uint32_t receive_endpoint,
					const int8_t* buffer,
					uint32_t buffer_size)
{
	uint32_t bridge_base;
	uint16_t NS_serviceID = SEND_DATA_TO_REMOTE_REQUEST;
	uint32_t msgSize = 24 + buffer_size;	// header size + payload size
	int i;

#ifdef NS_DEBUG_ON
	printf("NS_sendDataToRemote_request:\n"); fflush(stdout);
	printf("                  send_endpoint    = 0x%x\n", send_endpoint);
	printf("                  receive_endpoint = 0x%x\n", receive_endpoint);
	printf("                  buffer_size      = 0x%x\n", buffer_size);
	fflush(stdout);
#endif

	// get dest. endpoint IDs
	uint16_t rd,rn,re;
	mcapi_trans_decode_handle(receive_endpoint,&rd,&rn,&re);

	// check if destination node parameters are valid
	if(!nios_node_mapping_db[my_node_id].destination_nodes[rn].valid) {
		return(NS_ERROR);
	}

	// alignment of msg[] buffer forced because later 4 byte load
	// and store instructions may be used to copy buffer
	uint8_t msg[msgSize] __attribute__ ((aligned (4)));

	// Now request packet will be build
	pack_add_u32(msg,  0, my_domain_id);	 // Header: source domain ID
	pack_add_u32(msg,  4, my_node_id);		 // Header: source node ID
	pack_add_u16(msg,  8, NS_serviceID);	 // Header: Service-ID
	pack_add_u16(msg, 10, 0);				 // Header: callID

	pack_add_u32(msg, 12, send_endpoint);	 // Payload: Sending endpoint
	pack_add_u32(msg, 16, receive_endpoint); // Payload: Receiving endpoint
	pack_add_u32(msg, 20, buffer_size);		 // Payload: size of payload area
#ifdef LINUX
	for(i = 0; i < buffer_size; i++) {		 // Payload: now payload area will be copied
		msg[24+i] = buffer[i];
	}
#endif

	// Find out the route, i.e. the base address of the FIFO bridge
	bridge_base = nios_node_mapping_db[my_node_id].destination_nodes[rn].base;

	// Send packet using PI layer service
#ifdef UCOSII
	PH_send_request(bridge_base, (uint32_t *)msg, 24, (uint32_t *)buffer, (uint32_t) buffer_size);
#endif

#ifdef LINUX
	int index = getIndex(bridge_base); //find out the right index for filedescriptor

#ifdef FIFO
	if(write(filedescriptor[index], msg, msgSize) != msgSize) {
		printf("NS_sendDataToRemote_request: sending data failed");
		return(NS_ERROR);
	}
#endif // FIFO

#ifdef SOCK
	if(PH_TCPSock_send(msg, msgSize) != msgSize) {
		printf("NS_sendDataToRemote_request: sending data failed");
		return(NS_ERROR);
	}
#endif // SOCK

#endif // LINUX
	return(NS_OK);	// ret an Okay!
}

/*************************************************************
 * FUNCTION: NS_sendDataToRemote_indication()
 *
 * DESCRIPTION:
 * If a remote instance has called service 'sendDataToRemote'
 * by calling NS_sendDataToRemote_request() this will lead
 * to a call of this function. Function will receive the
 * message and will transfer it to the upper layer
 * software, which is the MCAPI_trans layer.
 *
 * INPUT PARAMETERS:
 *   - send_endpoint:    source endpoint
 *   - receive_endpoint: destination endpoint
 *   - buffer:           pointer to message location
 *   - buffer_size:      message size in bytes
 *
 * RETURN VALUE: -
 *************************************************************/
void NS_sendDataToRemote_indication(uint32_t bridge_base, NS_pduA *packet)
{
	// extract parameters send by remote node
	uint32_t send_endpoint = pack_get_u16(packet->payload, 0);
	uint32_t receive_endpoint = pack_get_u16(packet->payload, 4);;
	uint32_t buffer_size = pack_get_u32(packet->payload, 8);
	uint8_t *buffer = (packet->payload) + 12;

#ifdef NS_DEBUG_ON
	printf("NS_sendDataToRemote_indication: send_endpoint    = 0x%x\n", send_endpoint);
	printf("                                receive_endpoint = 0x%x\n", receive_endpoint);
	printf("                                buffer_size      = 0x%x\n", buffer_size);
#endif

	// get source and dest. endpoint IDs
	uint16_t sd,sn,se;
	uint16_t rd,rn,re;
	mcapi_trans_decode_handle(send_endpoint,&sd,&sn,&se);
	mcapi_trans_decode_handle(receive_endpoint,&rd,&rn,&re);

	// If remote node is up before we are up, we have to wait
	// until MCAPI_Transport layer is fully initialized.
	while(mcapi_trans_initialized() != MCAPI_TRUE)
		usleep(TIM_DEL1_MS * 1000);	// wait until MCAPI is initialized

	// error return MCAPI_FALSE of mcapi_trans_send() normally indicates that
	// temporally no receive buffer is available, ie. we have to wait a little
	// bit and have to try again later.

#ifdef NS_DEBUG_ON
	int i;
	printf("NS_sendDatatoRemote_indication: received data = ");
	for(i = 0; i < buffer_size; i++)
		printf("0x%x, ", buffer[i]);
	printf("\n");
#endif

	while(mcapi_trans_send(sd, sn, se, rd, rn, re, (uint8_t *) buffer, buffer_size) != MCAPI_TRUE)
		usleep(TIM_DEL1_MS * 1000);	// wait for 1 tick
}

/*************************************************************
 * FUNCTION: NS_unlock_waiting_request()
 *
 * DESCRIPTION:
 * If we have received a response from a remote node the
 * request-function waiting for this response will be unlocked.
 *
 * INPUT PARAMETERS:
 *  data: pointer to the response packet
 *
 * RETURN VALUE: -
 ************************************************************/
void NS_unlock_waiting_request(NS_pduA *packet)
{
	int	i, d;
	uint8_t err;

	// Check which function is waiting for this
	// return, copy payload data to return data structure
	// post the corresponding semaphore
	for(i = 0; i < MAX_NS_SERVICE_REQUESTS; i++) {
		if(serviceRequest[i].callID == packet->callID) {
			serviceRequest[i].syncResponse.payloadLength = packet->payloadLength;
			serviceRequest[i].syncResponse.NS_serviceID = packet->NS_serviceID;

			for(d = 0; d < packet->payloadLength; d++) {	// copy payload area
				serviceRequest[i].syncResponse.payload[d] = packet->payload[d];
			}
#ifdef UCOSII
			err = OSSemPost(serviceRequest[i].semSync);
						if(err != OS_NO_ERR) {
							printf("NS_unlock_waiting_request: Error in OSSemPost: %i\n",err);
						}
#endif
#ifdef LINUX
			err = sem_post(serviceRequest[i].semSync);
			if(err != 0) {
				printf("NS_unlock_waiting_request: Error in sem_post(): %i\n",err);
			}
#endif
		}
	}
}

/*************************************************************
 * FUNCTION: NS_layer_receive()
 *
 * DESCRIPTION:
 * PH layer calls this function in case of a message receipt.
 * Function acts as a service demultiplexer. It will analyze
 * the passed packet and will pass it to corresponding
 * service function.
 *
 * INPUT PARAMETERS:
 *  *  data: pointer to the response packet
 *
 * RETURN VALUE:
 ************************************************************/
void NS_layer_receive(PH_pdu *data)
{
    // Extract NS layer specific parameters
    NS_pduA packet;
    packet.payloadLength = data->length - 12;
	packet.srcDomain 	= pack_get_u32(&(data->data[0]),  0);
	packet.srcNode   	= pack_get_u32(&(data->data[0]),  4);
	packet.NS_serviceID	= pack_get_u16(&(data->data[0]),  8);
	packet.callID		= pack_get_u16(&(data->data[0]), 10);
    packet.payload = (data->data) + 12;

#ifdef NS_DEBUG_ON
	printf("NS_layer_receive: srcDomain     = %d\n", packet.srcDomain);
	printf("                  srcNode       = %d\n", packet.srcNode);
	printf("                  NS_serviceID  = %d\n", packet.NS_serviceID);
	printf("                  callID        = %d\n", packet.callID);
	printf("                  payloadLength = %d\n", packet.payloadLength);
	fflush(stdout);
#endif

    switch(packet.NS_serviceID)
    {
    case SEND_DATA_TO_REMOTE_REQUEST:
    	NS_sendDataToRemote_indication(data->bridge_base, &packet);
	break;
    case GET_REMOTE_ENDPOINT_REQUEST:
    	NS_getRemoteEndpoint_response(data->bridge_base, &packet);
    	break;
    case ENDPOINT_CHANNEL_ISOPEN_REQUEST:
    	NS_endpointChannelIsopen_response(data->bridge_base, &packet);
	break;
    case GET_REMOTE_ENDPOINT_RESPONSE:
    case ENDPOINT_CHANNEL_ISOPEN_RESPONSE:
		NS_unlock_waiting_request(&packet);
    	break;
    default:
    	printf("NS_layer_receive: unknown service or response ID\n");
    }
}
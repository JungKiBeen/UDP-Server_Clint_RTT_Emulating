 #define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define Q_SIZE 10000
#define PKT_SIZE 1000
#define ALPHA 0.125

#define UPDATE_AVGRTT(x) (avgRTT = (1-ALPHA)*avgRTT + ALPHA*(x)) // sec
#define ELAPSED_TIME(s,f) ((double)(f) - (s))/1000

#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <process.h>
#include <Windows.h>
#include <string.h>
#include <time.h>
#pragma comment(lib, "ws2_32.lib")

typedef clock_t data; // ms 

/*함수 선언*/
typedef struct queue {
	int head;
	int tail;
	int cnt;
	data ar[Q_SIZE];
}QUEUE;

void enqueue(QUEUE *q, data x);
data dequeue(QUEUE *q);
int queue_empty(QUEUE* q);
int queue_full(QUEUE* q);

UINT WINAPI recThread_func(void* para);
UINT WINAPI printThread_func(void* para);
void incr_acksnum();
void half_sendingrate();
void incr_sendingrate();
void error_handle(char* message);
BOOL try_connect();


/*변수 선언*/
HANDLE recThread, printThread;
UINT recThreadID, printThreadID;

WSADATA wsa_data;
SOCKET sock;
SOCKADDR_IN send_addr;

// 공유 메모리
CRITICAL_SECTION cs_ack, cs_sdrate, cs_avrtt, cs_que;
int acks_num;
double sending_rate; // sec
double avgRTT;
QUEUE que;

int pps;
BOOL is_first_rtt = TRUE;


int main(void)
{
	int pps;
	que.cnt = 0;
	que.head = 0;
	que.tail = 0;

	InitializeCriticalSection(&cs_ack);    // cs 생성
	InitializeCriticalSection(&cs_sdrate);    // cs 생성
	InitializeCriticalSection(&cs_avrtt);    // cs 생성
	InitializeCriticalSection(&cs_que);    // cs 생성

	printf("Input initial_sending_rate >>");
	scanf("%d", &pps);
	sending_rate = (double)pps;

	// 소켓 라이브러리 초기화
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
		error_handle("WSAStartup() errer!");

	// 소켓 생성
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET)
		error_handle("socket() error!");
	memset(&send_addr, 0, sizeof(send_addr));
	send_addr.sin_family = AF_INET;
	send_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	send_addr.sin_port = htons(12000);


	// UDP Connection 

	if (!try_connect()) 
	{
		printf("Receiver가 접속해 있지 않습니다. Receiver를 먼저 접속시켜주세요!\n");
		return 0;
	}


	recThread = (HANDLE)_beginthreadex(NULL, 0, recThread_func, 0, 0, &recThreadID);
	printThread = (HANDLE)_beginthreadex(NULL, 0, printThread_func, 0, 0, &printThreadID); // print Thread


	/* send 메인 스레드*/
	while (1)
	{
		int retval;
		clock_t _timer = clock();
		while (ELAPSED_TIME(_timer, clock()) < (1/sending_rate)); // minimum RTT 동안 지연한다

		char send_buf[PKT_SIZE + 1] = ".";
		enqueue(&que, clock());

		retval = sendto(sock, send_buf, PKT_SIZE, 0, (SOCKADDR*)&send_addr, sizeof(send_addr)); // 패킷 전송
		if (retval == SOCKET_ERROR)
			continue;
	}

	CloseHandle(recThread);
	CloseHandle(printThread);
	closesocket(sock);
	WSACleanup();

	DeleteCriticalSection(&cs_avrtt);      // cs 제거
	DeleteCriticalSection(&cs_ack);      // cs 제거
	DeleteCriticalSection(&cs_sdrate);      // cs 제거
	DeleteCriticalSection(&cs_que);      // cs 제거

	return 0;
}

void error_handle(char* message)
{
	fputs(message, stderr);
	fputc('\n', stderr);
	exit(1);
}

void incr_sendingrate()
{
	EnterCriticalSection(&cs_sdrate);           // lock 획득 혹은 waiting
	sending_rate = sending_rate + 1.0/ sending_rate;
	LeaveCriticalSection(&cs_sdrate);           // lock 반환
}

void half_sendingrate()
{
	EnterCriticalSection(&cs_sdrate);           // lock 획득 혹은 waiting
	sending_rate = sending_rate / 2.0;
	LeaveCriticalSection(&cs_sdrate);           // lock 반환
}

void incr_acksnum()
{
	EnterCriticalSection(&cs_ack);           // lock 획득 혹은 waiting
	acks_num++;
	LeaveCriticalSection(&cs_ack);           // lock 반환
}

int queue_full(QUEUE* q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	int ret = ((q->head + 1) % Q_SIZE == q->tail);
	LeaveCriticalSection(&cs_que);           // lock 반환

	return ret;
}

/*
queue_empty( QUEUE)
This function check whether the queue is empty.
*/


int queue_empty(QUEUE* q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	int ret = (q->head == q->tail);
	LeaveCriticalSection(&cs_que);           // lock 반환

	return ret;
}

/*
enqueue( QUEUE *, int)
insert a data into the queue
*/
void enqueue(QUEUE *q, data x)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting

	if ((q->head + 1) % Q_SIZE == q->tail)
	{
		printf("the queue is full!\n");

		return;
	}

	q->head = (q->head + 1) % Q_SIZE;

	q->ar[q->head] = x;
	q->cnt++;
	LeaveCriticalSection(&cs_que);           // lock 반환

}


data dequeue(QUEUE *q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	data ret;

	if (q->head == q->tail)
	{
		printf("the queue is empty!\n");
		return 0;
	}

	q->tail = (q->tail + 1) % Q_SIZE;
	ret = q->ar[q->tail];
	q->cnt--;
	LeaveCriticalSection(&cs_que);           // lock 반환
	
	return ret;
}

/* print 스레드 */
UINT WINAPI printThread_func(void* para)
{
	clock_t _init, _start;

	_init = clock();
	_start = clock();
	while (1)
	{
		if (ELAPSED_TIME(_start, clock()) >= 2.0)
		{
			double good_put;
			double elapsed = ELAPSED_TIME(_init, clock());
			

			EnterCriticalSection(&cs_avrtt);           // lock 획득 혹은 waiting
			printf("\n[%lf]Average RTT : %lf\n", elapsed, avgRTT);
			LeaveCriticalSection(&cs_avrtt);           // lock 반환

			EnterCriticalSection(&cs_sdrate);           // lock 획득 혹은 waiting
			printf("[%lf]Sedning Rate : %lf pps\n", elapsed, sending_rate);
			LeaveCriticalSection(&cs_sdrate);           // lock 반환
			
			EnterCriticalSection(&cs_ack);           // lock 획득 혹은 waiting
			good_put = acks_num / 2.0;			 // data 접근 코드
			acks_num = 0;
			LeaveCriticalSection(&cs_ack);           // lock 반환

			printf("[%lf]Goodput : %lf\n\n", elapsed, good_put);
			
			half_sendingrate();
			_start = clock();
		}
	}
}

/* recevie 스레드*/
UINT WINAPI recThread_func(void* para)
{
	SOCKADDR_IN peer_addr;
	char rcv_buf[PKT_SIZE+1];
	int retval;
	while (1)
	{
		int addrlen = sizeof(peer_addr);
		retval = recvfrom(sock, rcv_buf, PKT_SIZE, 0, (SOCKADDR*)&peer_addr, &addrlen);
	
		if (retval == SOCKET_ERROR)
			continue;

		// 다른 ip로부터 패킷 수신
		if (memcmp(&peer_addr, &send_addr, sizeof(peer_addr)))
			continue;

		rcv_buf[retval] = '\0';

		if (!strcmp(rcv_buf, "ACK"))
		{
			incr_sendingrate(); // 1.sending_rate를 증가시킨다
			double sample_rtt = ELAPSED_TIME(dequeue(&que), clock());

			if (is_first_rtt)
			{
				EnterCriticalSection(&cs_avrtt);           // lock 획득 혹은 waiting
				avgRTT = sample_rtt;
				LeaveCriticalSection(&cs_avrtt);           // lock 반환
				is_first_rtt = FALSE;
			}
			else
			{
				EnterCriticalSection(&cs_avrtt);
				UPDATE_AVGRTT(sample_rtt);					// 2. avg_RTT 업데이트 
				LeaveCriticalSection(&cs_avrtt);
			}
			incr_acksnum();	// 3. 2초 동안의 acks_num 증가

		}

		else if (!strcmp(rcv_buf, "NACK"))
		{
			half_sendingrate();

		}

	}
}

BOOL try_connect()
{
	char send_buf[PKT_SIZE+1], rcv_buf[PKT_SIZE+1];
	int retval, addrlen;

	strcpy(send_buf, "connect");

	retval = sendto(sock, send_buf, (int)strlen(send_buf), 0, (SOCKADDR*)&send_addr, sizeof(send_addr));
	if (retval == SOCKET_ERROR)
		return FALSE;

	addrlen = sizeof(send_addr);
	retval = recvfrom(sock, rcv_buf, PKT_SIZE, 0, (SOCKADDR*)&send_addr, &addrlen);

	if (retval == SOCKET_ERROR)
		return FALSE;

	rcv_buf[retval] = '\0';

	if (!strcmp(rcv_buf, "accept")) return TRUE;
	else return FALSE;
}

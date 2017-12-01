#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define PKT_SIZE 1000
#define TRUE 1
#define FALSE 0

#define ELAPSED_TIME(s,f) ((double)(f) - (s))/1000


#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <process.h>
#include <Windows.h>
#include <string.h>
#include <time.h>
#pragma comment(lib, "ws2_32.lib")

typedef SOCKADDR_IN  data; // ms 
typedef int bool;

/*함수 선언*/
typedef struct queue {
	int head;
	int tail;
	int cnt;
	data* ar;
}QUEUE;

void error_handle(char* message);
void enqueue(QUEUE *q, data x);
data dequeue(QUEUE *q);
int queue_empty(QUEUE* q);
int queue_full(QUEUE* q);
int get_qcnt(QUEUE *q);
UINT WINAPI printThread_func(void* para);
UINT WINAPI emulThread_func(void* para);
void incr_ack_num();

/*변수 선언*/
HANDLE emulThread, printThread;
UINT emulThreadID, printThreadID;

WSADATA wsa_data;
SOCKET sock;
SOCKADDR_IN send_addr, test_addr;

// 공유 메모리
CRITICAL_SECTION cs_ack, cs_que;
int acks_num;
QUEUE que;

double bottleneck_rate, minimum_rtt; // sec
int pps, bottolneck_qsize;



int main(void)
{
	que.cnt = 0;
	que.head = 0;
	que.tail = 0;
	InitializeCriticalSection(&cs_ack);    // cs 생성
	InitializeCriticalSection(&cs_que);    // cs 생성

	printf("Input minimum_RTT >>");
	scanf("%lf", &minimum_rtt);

	printf("Input bottleneck_link_rate >>");
	scanf("%d", &pps);
	bottleneck_rate = 1 / (double)pps;

	printf("Input bottleneck_queue_size >>");
	scanf("%d", &bottolneck_qsize);
	que.ar = (data*)malloc(bottolneck_qsize * sizeof(data)); // 큐 사이즈 결정

	// 소켓 라이브러리 초기화
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
		error_handle("WSAStartup() error!");

	// 소켓 생성
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET)
		error_handle("socket() error!");

	// 어드레스 설정
	memset(&send_addr, 0, sizeof(send_addr));
	send_addr.sin_family = AF_INET;
	send_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	send_addr.sin_port = htons(12000);

	if (bind(sock, (SOCKADDR*)&send_addr, sizeof(send_addr)) == SOCKET_ERROR)
		error_handle("bind() error!");

	emulThread = (HANDLE)_beginthreadex(NULL, 0, emulThread_func, 0, 0, &emulThreadID);
	printThread = (HANDLE)_beginthreadex(NULL, 0, printThread_func, 0, 0, &printThreadID); // print Thread


	int retval;
	clock_t _timer = clock();

	// Recevie 메인 스레드
	while (1)
	{
		if (ELAPSED_TIME(_timer, clock()) >= bottleneck_rate)
		{
			if (!queue_empty(&que))
			{
				SOCKADDR_IN recv_addr = dequeue(&que); // bottleneck 큐에서 데이터를 꺼낸다

				_timer = clock();
				while (ELAPSED_TIME(_timer, clock()) < minimum_rtt); // minimum RTT 동안 지연한다

				char send_buf[PKT_SIZE + 1] = "ACK";
				retval = sendto(sock, send_buf, strlen(send_buf), 0, (SOCKADDR*)&recv_addr, sizeof(recv_addr));

				incr_ack_num();
			}
			_timer = clock(); // 타이머 셋
		}
	}

	CloseHandle(emulThread);
	CloseHandle(printThread);

	closesocket(sock);
	WSACleanup();

	DeleteCriticalSection(&cs_ack);      // cs 제거
	DeleteCriticalSection(&cs_que);      // cs 제거

	return 0;
}

void incr_ack_num()
{
	EnterCriticalSection(&cs_ack);           // lock 획득 혹은 waiting
	acks_num++;
	LeaveCriticalSection(&cs_ack);           // lock 반환
}

void error_handle(char* message)
{
	fputs(message, stderr);
	fputc('\n', stderr);
	exit(1);
}

int queue_full(QUEUE* q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	int ret = ((q->head + 1) % bottolneck_qsize == q->tail);
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

	if ((q->head + 1) % bottolneck_qsize == q->tail)
	{
		printf("the queue is full!\n");

		return;
	}

	q->head = (q->head + 1) % bottolneck_qsize;

	q->ar[q->head] = x;
	q->cnt++;
	LeaveCriticalSection(&cs_que);           // lock 반환
}

int get_qcnt(QUEUE *q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	int ret = q->cnt;
	LeaveCriticalSection(&cs_que);           // lock 반환
	return ret;
}

data dequeue(QUEUE *q)
{
	EnterCriticalSection(&cs_que);           // lock 획득 혹은 waiting
	data ret;

	if (q->head == q->tail)
	{
		printf("the queue is empty!\n");
		return send_addr;
	}

	q->tail = (q->tail + 1) % bottolneck_qsize;
	ret = q->ar[q->tail];
	q->cnt--;
	LeaveCriticalSection(&cs_que);           // lock 반환

	return ret;
}

UINT WINAPI emulThread_func(void* para)
{
	SOCKADDR_IN peer_addr;
	char rcv_buf[PKT_SIZE + 1];
	int retval;

	while (1)
	{
		int addrlen = sizeof(peer_addr);
		retval = recvfrom(sock, rcv_buf, PKT_SIZE, 0, (SOCKADDR*)&peer_addr, &addrlen);

		if (retval == SOCKET_ERROR)
			continue;
		rcv_buf[retval] = '\0';

		if (!strcmp(rcv_buf, "connect"))
		{
			char send_buf[PKT_SIZE + 1] = "accept";
			retval = sendto(sock, send_buf, (int)strlen(send_buf), 0, (SOCKADDR*)&peer_addr, sizeof(peer_addr));
			if (retval == SOCKET_ERROR) continue;
		}

		else
		{
			// 큐가 꽉차면 수신한 peer_Addr로 NACK 즉시 회신
			if (queue_full(&que))
			{
				clock_t _timer = clock();
				while (ELAPSED_TIME(_timer, clock()) < minimum_rtt); // minimum RTT 동안 지연한다

				char send_buf[PKT_SIZE + 1] = "NACK";
				retval = sendto(sock, send_buf, (int)strlen(send_buf), 0, (SOCKADDR*)&peer_addr, sizeof(peer_addr));
				if (retval == SOCKET_ERROR) continue;
			}

			else
			{
				enqueue(&que, peer_addr);
			}
		}
	}

}

UINT WINAPI printThread_func(void* para)
{
	clock_t _init, _timer;
	int t;
	int sum, qoc[20];

	_init = clock();
	_timer = clock();
	t = 0;
	// Recevie 메인 스레드
	while (1)
	{
		if (ELAPSED_TIME(_timer, clock()) >= 0.1)
		{
			qoc[t] = get_qcnt(&que);
			t++;
			_timer = clock();
		}

		if (t == 20)
		{
			double elapsed = ELAPSED_TIME(_init, clock());

			EnterCriticalSection(&cs_ack);           // lock 획득 혹은 waiting
			printf("\n[%lf]Receiving Rate : %lf\n", elapsed, acks_num / 2.0);
			acks_num = 0;
			LeaveCriticalSection(&cs_ack);           // lock 반환
			sum = 0;
			for (int i = 0; i < 20; i++)
				sum += qoc[i];

			printf("[%lf]Average Queue Occupancy : %lf\n\n", elapsed, sum / 20.0);
			t = 0;
		}
	}
}





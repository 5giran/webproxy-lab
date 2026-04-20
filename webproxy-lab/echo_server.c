/*
 * echo_server.c - 학습용 echo 서버 골격 코드.
 *
 * webproxy-lab 디렉터리에서 직접 빌드:
 *   gcc -g -Wall echo_server.c csapp.c -o echo_server -lpthread
 *
 * 실행:
 *   ./echo_server <port>
 */
#include "csapp.h"

static void echo(int connfd);
int my_open_clientfd(char *hostname, char *port);
int my_open_listenfd(char *port);
int my_client_main(int argc, char **argv);

/*
 * TODO 1: 구현 - echo 서버 - my_open_clientfd()
 *
 * 목표:
 *   hostname과 port를 받아서 해당 서버로 연결된 clientfd를 반환한다.
 *   소켓 만들고 → hostname:port 서버에 TCP 연결까지 완료한 (소켓)fd 반환
 *   
 *   즉, 특정 서버와 연결하는 함수
 * 
 * 구현 순서:
 *   1. addrinfo hints 구조체를 준비한다.
 *      - ai_socktype은 SOCK_STREAM으로 설정한다.
 *      - ai_flags에는 AI_NUMERICSERV, AI_ADDRCONFIG 등을 설정한다.
 *   2. getaddrinfo(hostname, port, &hints, &listp)를 호출한다.
 *   3. 반환된 주소 리스트를 순회한다.
 *      - socket()으로 소켓을 만든다.
 *      - connect()로 서버 연결을 시도한다.
 *      - connect()가 성공하면 반복을 멈춘다.
 *      - 실패하면 해당 소켓을 close()한다.
 *   4. freeaddrinfo(listp)로 주소 리스트를 해제한다.
 *   5. 연결에 성공했으면 clientfd를 반환하고, 실패했으면 -1을 반환한다.
 */
int my_open_clientfd(char *hostname, char *port)
{
    // clientfd = 실제로 연결된 소켓
    int clientfd; /* 반환할 파일 디스트립터를 담는 변수, socket()이 반환하는 fd를 여기 저장하고, 마지막에 return clinetfd한다. */ 
    struct addrinfo hints, *listp, *p; /* 소켓의 종류-속성 담는 구조체, 결과 주소 linked list의 head 포인터 (이 hostname에 연결할 수 있는 주소들의 목록: 여러개 나올 수 있어서 lisked list) */

    // hints 구조체 구성
    memset(&hints, 0, sizeof(struct addrinfo)); // 구조체 전체 0으로 초기화
    hints.ai_socktype = SOCK_STREAM; // 구조체 필드 채우기 (소켓 특성으로)
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_flags |= AI_ADDRCONFIG;
    Getaddrinfo(hostname, port, &hints, &listp); // 필드까지 다 채운 후 Getaddrinfo 함수로 넘김

    /* 반환된 주소 리스트 순회 = inked list를 처음부터 끝까지 순회
     * p를 listp로 시작해서, p가 NULL이 아닌 동안, 매 반복마다 p를 다음 노드로 이동한다
     */
    for (p = listp; p; p = p->ai_next) {
        // 소켓 만들기 실패했을때는 바로 다음 노드로
        if ((clientfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;

        // connect() 성공하면 반복을 멈춘다. 
        if (connect(clientfd, p->ai_addr, p->ai_addrlen) != -1)
            break;
        // 실패했을때는 clientfd-소켓을 아예 닫아버려야한다. 연결 실패한 소켓이 열려있으면 안된다.
        Close(clientfd);
    }

    // connect()가 성공하면 그 주소 정보는 이미 소켓에 쓰여있음. 실패하면 그 메모리는 필요가 없음.
    // 그러므로 포인터 listp를 해제
    Freeaddrinfo(listp);
    // 연결에 실패했으면 -1반환, 성공했으면 clientfd 반환
    if (!p)
        return -1;
    else
        return clientfd;
}

/*
 * TODO 2: 구현 - echo 서버 - my_open_listenfd()
 *
 * 목표:
 *   port를 받아서 클라이언트 연결을 기다리는 listenfd를 반환한다.
 *
 * 구현 순서:
 *   1. addrinfo hints 구조체를 준비한다.
 *      - ai_socktype은 SOCK_STREAM으로 설정한다.
 *      - ai_flags에는 AI_PASSIVE, AI_ADDRCONFIG, AI_NUMERICSERV를 설정한다.
 *   2. getaddrinfo(NULL, port, &hints, &listp)를 호출한다.
 *   3. 반환된 주소 리스트를 순회한다.
 *      - socket()으로 소켓을 만든다.
 *      - setsockopt()로 SO_REUSEADDR 옵션을 켠다.
 *      - bind()로 소켓을 port에 연결한다.
 *      - bind()가 성공하면 반복을 멈춘다.
 *      - 실패하면 해당 소켓을 close()한다.
 *   4. freeaddrinfo(listp)로 주소 리스트를 해제한다.
 *   5. listen()으로 listenfd를 연결 대기 상태로 만든다.
 *   6. 성공하면 listenfd를 반환하고, 실패하면 -1을 반환한다.
 */
int my_open_listenfd(char *port)
{
    int listenfd, optval = 1;
    struct addrinfo hints, *listp, *p;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_flags |= AI_ADDRCONFIG;
    hints.ai_flags |= AI_NUMERICSERV;
    
    // 이미 연결이 선행되었으므로 해당 포트로 오는 요청 다 listen하면 됨. 그래서 hostname 지정이 필요 없음.
    getaddrinfo(NULL, port, &hints, &listp);

    for (p=listp; p; p=p->ai_next) {
        if ((listenfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
            continue;
        // 여기까지 이전 서버와 같음

        // 소켓 세부 설정 함수
        // 이 소켓에 SO_REUSEADDR(TIME_WAIT 상태인 포트도 재사용) 옵션을 켜줘
        setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(listenfd);
    }
    freeaddrinfo(listp);
    if (!p)
        return -1;
        // listen(연결대기하는소켓, 대기열의 최대 크기-1024로 정의)
    if (listen(listenfd, LISTENQ) < 0) { // listen은이 소켓으로 클라이언트 연결 받을 준비 됐어 라고 OS에 알려주는 것
        close(listenfd); // OS가 이 소켓을 대기 상태로 만들어줄수 없는 경우 바로 닫음
        return -1;
    }
    return listenfd;
}

/*
 * TODO 3: 구현 - echo 서버 - 클라이언트 main()
 *
 * 목표:
 *   사용자가 입력한 문장을 서버로 보내고, 서버가 되돌려준 문장을 출력한다.
 *
 * 구현 순서:
 *   1. argc가 3인지 확인한다.
 *      - argv[1]은 서버 hostname이다.
 *      - argv[2]는 서버 port이다.
 *   2. my_open_clientfd(hostname, port)로 서버에 연결한다.
 *   3. Rio_readinitb로 서버 소켓에 대한 rio_t를 초기화한다.
 *   4. 표준 입력에서 Fgets로 한 줄씩 읽는다.
 *   5. Rio_writen으로 서버에 입력 내용을 보낸다.
 *   6. Rio_readlineb로 서버가 echo한 응답을 읽는다.
 *   7. Fputs로 응답을 표준 출력에 출력한다.
 *   8. 입력이 끝나면 close(clientfd)로 연결을 닫는다.
 */
int my_client_main(int argc, char **argv)
{
    int clientfd;
    char *host, *port, buf[MAXLINE];
    rio_t rio;

    if (argc != 3) {
        // 프로그램 올바른 사용법 알려주고 프로그램 종료
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0);
    }
    host = argv[1];
    port = argv[2];

    // 서버에 연결해서 소켓 fd를 얻어옴
    // clientfd = 서버와 연결된 통로
    clientfd = my_open_clientfd(host, port);
    // rio 라는 읽기용 구조체를 clientfd에 연결 (rio)
    // 앞으로 rio를 이용해서 clientfd, 즉 서버가 보내는 데이터를 buffered 방식으로 읽겠다.
    Rio_readinitb(&rio, clientfd);

    // 사용자 입력을 한 줄씩 읽어오는 반복문
    while (Fgets(buf, MAXLINE, stdin) != NULL) {
        // 사용자가 입력한 내용을 서버로 전송
        // Rio_writen: fd에 주어진 n바이트를 끝까지 쓰는 함수(루프돌림)
        //             여기서는 clientfd가 서버와 연결된 소켓 fd라서, 그 fd에 쓰면 데이터가 서버로 가는 것
        Rio_writen(clientfd, buf, strlen(buf));
        // 서버가 echo해서 돌려준 응답을 buf애 저장
        Rio_readlineb(&rio, buf, MAXLINE);
        // 서버 응답을 화면에 출력
        Fputs(buf, stdout);
    }
    close(clientfd);
    exit(0);

}

int main(int argc, char **argv)
{
    /*
     * TODO:
     * 1. my_open_listenfd(argv[1])로 listen 소켓을 연다.
     * 2. 반복문 안에서 클라이언트 연결을 accept한다.
     * 3. echo(connfd)를 호출한 뒤 connfd를 닫는다.
     */
    // listenfd는 연결 요청을 계속 기다리고, 클라이언트가 오면 connfd를 새로 만들어서 그 클라이언트랑 통신
    int listenfd, connfd; // 접속을 기다리는 서버 소켓, 실제 클라이언트와 통신하는 소켓
    socklen_t clientlen; // 클라이언트 주소 구조체 크기
    struct sockaddr_storage clientaddr; // 클라이언트 주소 정보
    char client_hostname[MAXLINE], client_port[MAXLINE]; // IP, Port

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = my_open_listenfd(argv[1]); // 포트번호로 서버 소켓열기
    // 서버 끄기 전까지 계속 도는 무한루프
    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        // 클라이언트가 연결 요청을 보내면 수락하고, 클라이언트 전용 새 소켓(connfd)반환
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        // 클라이언트의 IP 주소와 포트번호를 사람이 읽을 수 있는 문자열로 변환해주는 함수
        Getnameinfo((SA *) &clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_hostname, client_port);
        echo(connfd); // 에코서버 핵심: 클라이언트가 보낸 데이터를 그대로 돌려줌
        close(connfd);
    }
    exit(0);
}

static void echo(int connfd)
{
    /*
     * TODO 4: 구현 - echo 서버 - 서버 echo()
     *
     * 목표:
     *   클라이언트가 보낸 데이터를 읽고, 같은 데이터를 다시 클라이언트에게 보낸다.
     *
     * 구현 순서:
     *   1. rio_t rio 변수를 선언한다.
     *   2. Rio_readinitb(&rio, connfd)로 rio를 초기화한다.
     *   3. Rio_readlineb(&rio, buf, MAXLINE)로 한 줄씩 읽는다.
     *   4. 읽은 바이트 수가 0보다 큰 동안 반복한다.
     *   5. Rio_writen(connfd, buf, n)으로 읽은 내용을 그대로 다시 보낸다.
     *   6. 필요하면 서버 터미널에 받은 바이트 수와 내용을 출력한다.
     */
    size_t n;
    char buf[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, connfd);
    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        printf("server received %d bytes \n", (int)n);
        Rio_writen(connfd, buf, n);
    }
}

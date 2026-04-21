/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"
#include <strings.h>

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

/*
 * Tiny implementation guide
 * -------------------------
 *
 * Echo server에서 이미 배운 뼈대:
 *
 *   listenfd = Open_listenfd(port);
 *   while (1) {
 *     connfd = Accept(listenfd, ...);
 *     echo(connfd);
 *     Close(connfd);
 *   }
 *
 * Tiny도 네트워크 뼈대는 완전히 같다. 차이는 echo(connfd) 자리에
 * doit(connfd)가 들어간다는 점이다.
 *
 *   echo(connfd):
 *     클라이언트가 보낸 줄을 읽고 그대로 다시 쓴다.
 *
 *   doit(connfd):
 *     클라이언트가 보낸 HTTP 요청을 읽고, 해석하고, 알맞은 HTTP 응답을 쓴다.
 *
 * 따라서 Tiny를 구현할 때는 "소켓을 새로 배운다"기보다, connfd 위에서
 * 오가는 텍스트 약속인 HTTP를 처리한다고 생각하면 된다.
 *
 * 구현 순서 추천:
 *
 *   1. doit()
 *      요청 라인("GET /home.html HTTP/1.1")을 읽고 method, uri, version을
 *      나눈다. GET이 아니면 clienterror()로 거절한다.
 *
 *   2. read_requesthdrs()
 *      HTTP 요청 헤더를 빈 줄까지 읽어 버린다. Echo server처럼 EOF까지
 *      기다리면 안 된다. HTTP 요청 헤더의 끝은 빈 줄("\r\n")이다.
 *
 *   3. parse_uri()
 *      uri가 정적 컨텐츠인지 동적 CGI인지 판단하고, 실제 파일 경로와
 *      cgiargs를 만든다.
 *
 *   4. clienterror()
 *      404, 403, 501 같은 에러도 HTTP 응답 형식으로 보내는 연습을 한다.
 *
 *   5. serve_static()
 *      정적 파일을 열고, Content-length / Content-type 헤더를 보낸 뒤,
 *      파일 내용을 클라이언트에게 쓴다.
 *
 *   6. serve_dynamic()
 *      CGI 프로그램을 Fork/Execve로 실행하고, 프로그램의 표준 출력을
 *      클라이언트 소켓 fd로 연결한다.
 *
 * 구현할 때 계속 확인할 질문:
 *
 *   - 지금 읽는 fd는 connfd인가, 파일 fd인가?
 *   - 지금 쓰는 fd는 connfd인가, 표준 출력인가?
 *   - HTTP에서 헤더와 본문 사이의 빈 줄을 보냈는가?
 *   - 정적 파일은 읽기 권한, CGI 파일은 실행 권한을 검사했는가?
 *   - 에러 상황도 클라이언트가 이해할 수 있는 HTTP 응답으로 보냈는가?
 */

int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /*
   * 에코서버 main과 동일
   * echo 함수 대신 doit 함수를 호출하는 것만 다름
   */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  /*
   * listenfd는 "연결 요청을 기다리는 문"이다.
   * 데이터를 주고받는 fd가 아니라, Accept()로 connfd를 만들어내는 fd다.
   */
  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);

    /*
     * connfd는 "이번 클라이언트와 실제로 대화하는 통로"다.
     * doit(connfd) 안에서 HTTP 요청을 읽고 응답을 쓴다.
     */
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen); // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);

    /*
     * echo server의 echo(connfd) 자리에 Tiny의 HTTP 처리기 doit(connfd)가
     * 들어간다고 보면 된다.
     */
    doit(connfd);  // line:netp:tiny:doit
    Close(connfd); // line:netp:tiny:close
  }
}

/*
 * doit - handle one HTTP request/response transaction
 *      - HTTP 요청 1건을 처리하고 응답을 반환한다 / 단일 HTTP 요청-응답 사이클을 처리한다
 *
 * Echo server의 echo(connfd)와 같은 자리의 함수다.
 * 단, echo는 "읽은 내용을 그대로 쓰기"였고, doit는 "HTTP 요청을 해석해서
 * HTTP 응답을 쓰기"다.
 *
 * 목표 흐름:
 *
 *   1. rio_t rio를 만들고 Rio_readinitb(&rio, fd)로 connfd에 연결한다.
 *   2. Rio_readlineb(&rio, buf, MAXLINE)로 요청 라인을 한 줄 읽는다.
 *
 *        예: "GET /home.html HTTP/1.1\r\n"
 *
 *   3. sscanf(buf, "%s %s %s", method, uri, version)로 세 부분을 나눈다.
 *      version은 당장 크게 쓰지 않지만, 요청 라인의 구조를 이해하기 위해
 *      받아 둔다.
 *
 *   4. method가 GET인지 검사한다.
 *      Tiny는 GET만 지원한다. GET이 아니면 clienterror()로 501을 보낸다.
 *
 *   5. read_requesthdrs(&rio)로 나머지 요청 헤더를 빈 줄까지 읽는다.
 *
 *   6. parse_uri(uri, filename, cgiargs)를 호출한다.
 *      반환값으로 정적 컨텐츠인지 동적 컨텐츠인지 구분한다.
 *
 *   7. stat(filename, &sbuf)로 파일 존재 여부와 메타데이터를 확인한다.
 *      실패하면 404 Not found를 보낸다.
 *
 *   8. 정적 컨텐츠라면:
 *      S_ISREG(sbuf.st_mode)와 S_IRUSR 권한을 확인한다.
 *      통과하면 serve_static(fd, filename, sbuf.st_size)를 호출한다.
 *      실패하면 403 Forbidden을 보낸다.
 *
 *   9. 동적 컨텐츠라면:
 *      S_ISREG(sbuf.st_mode)와 S_IXUSR 권한을 확인한다.
 *      통과하면 serve_dynamic(fd, filename, cgiargs)를 호출한다.
 *      실패하면 403 Forbidden을 보낸다.
 *
 * 구현 힌트:
 *
 *   - 필요한 지역 변수 후보:
 *       int is_static;
 *       struct stat sbuf;
 *       char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
 *       char filename[MAXLINE], cgiargs[MAXLINE];
 *       rio_t rio;
 *
 *   - 권한 검사는 bit mask다.
 *       sbuf.st_mode & S_IRUSR
 *       sbuf.st_mode & S_IXUSR
 *
 *   - 이 함수는 "한 클라이언트 연결에서 요청 하나를 처리한다"는 마음으로
 *     구현하면 된다. 처리가 끝나면 main()이 Close(connfd)를 해준다.
 */
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;

  /*
   * Step 1. connfd(fd)를 RIO 버퍼와 연결한다.
   *
   * fd는 main()에서 Accept()가 반환한 "이번 클라이언트 전용 소켓"이다.
   * 앞으로 이 fd에서 HTTP 요청을 읽고, 같은 fd로 HTTP 응답을 쓴다.
   */
  Rio_readinitb(&rio, fd);
  if (Rio_readlineb(&rio, buf, MAXLINE) <= 0)
    return;

  /*
   * Step 2. 요청 라인을 파싱한다.
   *
   * 클라이언트가 처음 보내는 한 줄은 보통 이렇게 생겼다.
   *
   *   GET /home.html HTTP/1.1
   *
   * method  -> GET
   * uri     -> /home.html
   * version -> HTTP/1.1
   */
  printf("Request line: %s", buf);
  if (sscanf(buf, "%s %s %s", method, uri, version) != 3)
  {
    clienterror(fd, buf, "400", "Bad request",
                "Tiny could not parse the request line");
    return;
  }

  /*
   * Step 3. Tiny가 지원하는 method인지 확인한다.
   *
   * 이 Tiny는 GET만 처리한다. POST/PUT 같은 요청은 "서버가 구현하지
   * 않은 기능"이므로 501 응답을 보낸다.
   */
  if (strcasecmp(method, "GET"))
  {
    clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
    return;
  }

  /*
   * Step 4. 남은 HTTP 요청 헤더를 끝까지 읽는다.
   *
   * 여기서 헤더 내용을 적극적으로 사용하지는 않지만, 읽지 않고 넘어가면
   * 소켓 안에 남은 데이터 때문에 다음 처리가 헷갈릴 수 있다.
   */
  read_requesthdrs(&rio);

  /*
   * Step 5. URI를 실제 서버 파일 경로로 바꾼다.
   *
   * 예:
   *   "/"                    -> "./home.html"
   *   "/home.html"           -> "./home.html"
   *   "/cgi-bin/adder?x=1&y=2" -> "./cgi-bin/adder", cgiargs="x=1&y=2"
   */
  is_static = parse_uri(uri, filename, cgiargs);
  if (stat(filename, &sbuf) < 0)
  {
    clienterror(fd, filename, "404", "Not found",
                "Tiny could not find this file");
    return;
  }

  /*
   * Step 6-A. 정적 컨텐츠 처리 경로.
   *
   * 정적 컨텐츠는 "파일을 그대로 읽어서 클라이언트에게 보내는" 경로다.
   * 그래서 regular file인지, 읽기 권한이 있는지를 확인한다.
   */
  if (is_static)
  {
    if (!S_ISREG(sbuf.st_mode) || !(sbuf.st_mode & S_IRUSR))
    {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny could not read this file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size);
    return;
  }

  /*
   * Step 6-B. 동적 컨텐츠 처리 경로.
   *
   * 동적 컨텐츠는 "CGI 프로그램을 실행해서 그 출력 결과를 보내는" 경로다.
   * 그래서 파일이 존재하는 것만으로는 부족하고, 실행 권한이 있어야 한다.
   */
  if (!S_ISREG(sbuf.st_mode) || !(sbuf.st_mode & S_IXUSR))
  {
    clienterror(fd, filename, "403", "Forbidden",
                "Tiny could not run this CGI program");
    return;
  }
  serve_dynamic(fd, filename, cgiargs);
}

/*
 * read_requesthdrs - read and optionally print HTTP request headers
 *
 * HTTP 요청은 보통 이런 모양이다.
 *
 *   GET /home.html HTTP/1.1\r\n
 *   Host: localhost:8000\r\n
 *   User-Agent: curl/...\r\n
 *   Accept: * / *\r\n
 *   \r\n
 *
 * 첫 줄은 doit()에서 이미 읽는다. 이 함수는 그 다음 줄들, 즉 header들을
 * 빈 줄까지 읽어 버리는 역할이다.
 *
 * Echo server와 가장 중요한 차이:
 *
 *   echo()는 Rio_readlineb가 0, 즉 EOF를 만날 때까지 읽었다.
 *   HTTP 서버는 EOF까지 기다리면 안 된다. 클라이언트가 연결을 닫기 전에
 *   서버 응답을 기다리고 있을 수 있기 때문이다.
 *
 *   HTTP 요청 헤더의 끝은 빈 줄이다. CSAPP Tiny에서는 보통 "\r\n"과
 *   비교한다.
 *
 * 구현 힌트:
 *
 *   char buf[MAXLINE];
 *
 *   Rio_readlineb(rp, buf, MAXLINE);
 *   while (strcmp(buf, "\r\n")) {
 *     필요하면 printf("%s", buf);
 *     Rio_readlineb(rp, buf, MAXLINE);
 *   }
 *
 * 학습 포인트:
 *
 *   - HTTP는 줄 기반 텍스트 프로토콜이다.
 *   - 요청 라인과 헤더는 Rio_readlineb로 읽기 좋다.
 *   - 헤더와 body 사이에는 반드시 빈 줄이 있다.
 */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];

  /*
   * 요청 헤더는 여러 줄이고, 마지막은 빈 줄이다.
   *
   * 중요:
   *   EOF까지 읽는 것이 아니라 "\r\n"까지만 읽는다.
   *   브라우저는 연결을 열어 둔 채 서버 응답을 기다릴 수 있기 때문이다.
   */
  printf("Request headers:\n");
  while (Rio_readlineb(rp, buf, MAXLINE) > 0)
  {
    printf("%s", buf);

    /* 빈 줄이면 HTTP 요청 헤더가 끝났다는 뜻이다. */
    if (!strcmp(buf, "\r\n") || !strcmp(buf, "\n"))
      break;
  }
}

/*
 * parse_uri - split URI into filename and CGI arguments
 *
 * URI는 요청 라인의 가운데 부분이다.
 *
 *   GET /home.html HTTP/1.1
 *       ^^^^^^^^^^
 *
 * Tiny에서는 URI를 두 종류로 나눈다.
 *
 *   정적 컨텐츠:
 *     /home.html
 *     /godzilla.gif
 *     /
 *
 *   동적 컨텐츠:
 *     /cgi-bin/adder?1&2
 *
 * 반환 규칙:
 *
 *   정적 컨텐츠면 1을 반환한다.
 *   동적 컨텐츠면 0을 반환한다.
 *
 * 정적 컨텐츠 구현 흐름:
 *
 *   1. cgiargs를 빈 문자열로 만든다.
 *   2. filename을 "."으로 시작하게 만든다.
 *      예: uri가 "/home.html"이면 filename은 "./home.html"
 *   3. uri가 "/"로 끝나면 "home.html"을 붙인다.
 *      예: "/" -> "./home.html"
 *
 * 동적 컨텐츠 구현 흐름:
 *
 *   1. uri에서 '?' 위치를 찾는다. strchr(uri, '?')를 떠올려 보자.
 *   2. '?'가 있으면 그 뒤를 cgiargs에 복사한다.
 *      예: "/cgi-bin/adder?1&2" -> cgiargs는 "1&2"
 *   3. '?' 자리를 '\0'으로 바꿔서 uri를 파일 경로 부분까지만 남긴다.
 *      예: uri는 "/cgi-bin/adder"가 된다.
 *   4. filename은 "." + uri로 만든다.
 *      예: "./cgi-bin/adder"
 *
 * 주의:
 *
 *   이 함수는 uri 문자열 자체를 수정해도 괜찮다. doit()에서 받은 uri는
 *   지역 배열이기 때문이다.
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *query;

  /*
   * 정적 컨텐츠 경로.
   *
   * "cgi-bin"이 없으면 실행할 프로그램이 아니라 서버 안의 파일을 달라는
   * 요청으로 본다. cgiargs는 쓰지 않으므로 빈 문자열로 둔다.
   */
  if (!strstr(uri, "cgi-bin"))
  {
    strcpy(cgiargs, "");

    /* 브라우저 URI "/home.html"을 Unix 파일 경로 "./home.html"로 바꾼다. */
    snprintf(filename, MAXLINE, ".%s", uri);

    /* "/" 요청은 Tiny의 기본 페이지인 "./home.html"로 연결한다. */
    if (strlen(uri) > 0 && uri[strlen(uri) - 1] == '/')
      strncat(filename, "home.html", MAXLINE - strlen(filename) - 1);
    return 1;
  }

  /*
   * 동적 컨텐츠 경로.
   *
   * "/cgi-bin/adder?x=1&y=2"에서 '?' 앞은 실행 파일 경로이고,
   * '?' 뒤는 CGI 프로그램에게 넘길 인자다.
   */
  query = strchr(uri, '?');
  if (query)
  {
    /* query + 1은 '?' 바로 다음 글자, 즉 "x=1&y=2"의 시작이다. */
    strcpy(cgiargs, query + 1);

    /* uri를 "/cgi-bin/adder"까지만 남기기 위해 '?'를 문자열 끝으로 만든다. */
    *query = '\0';
  }
  else
  {
    strcpy(cgiargs, "");
  }

  /* 실행할 CGI 프로그램의 실제 파일 경로를 만든다. */
  snprintf(filename, MAXLINE, ".%s", uri);
  return 0;
}

/*
 * serve_static - send a static file to the client
 *
 * 정적 컨텐츠는 서버 디렉터리에 이미 존재하는 파일이다.
 *
 *   /home.html      -> ./home.html
 *   /godzilla.gif  -> ./godzilla.gif
 *
 * 해야 할 일은 두 부분이다.
 *
 *   1. HTTP 응답 헤더 보내기
 *   2. 파일 내용 보내기
 *
 * 응답 헤더 예시:
 *
 *   HTTP/1.0 200 OK\r\n
 *   Server: Tiny Web Server\r\n
 *   Connection: close\r\n
 *   Content-length: 1234\r\n
 *   Content-type: text/html\r\n
 *   \r\n
 *
 * HTTP에서 빈 줄("\r\n")은 헤더가 끝나고 본문이 시작된다는 뜻이다.
 * 이 빈 줄을 빼먹으면 브라우저가 응답을 이상하게 해석할 수 있다.
 *
 * 파일 내용 보내기 흐름:
 *
 *   1. get_filetype(filename, filetype)로 Content-type을 정한다.
 *   2. Open(filename, O_RDONLY, 0)로 파일을 연다.
 *   3. Mmap으로 파일 내용을 메모리에 매핑한다.
 *   4. Close(srcfd)로 파일 fd는 닫는다.
 *   5. Rio_writen(fd, srcp, filesize)로 connfd에 파일 내용을 쓴다.
 *   6. Munmap(srcp, filesize)로 매핑을 해제한다.
 *
 * Echo server와 연결해서 보기:
 *
 *   echo는 클라이언트가 보낸 buf를 Rio_writen으로 되돌려 보냈다.
 *   serve_static은 파일 내용을 Rio_writen으로 클라이언트에게 보낸다.
 */
void serve_static(int fd, char *filename, int filesize)
{
  int srcfd;
  char *srcp;
  char filetype[MAXLINE], buf[MAXBUF];

  /*
   * Step 1. HTTP 응답 헤더를 먼저 보낸다.
   *
   * 브라우저는 헤더를 보고 뒤에 올 본문을 어떻게 해석할지 결정한다.
   * Content-length가 정확해야 파일을 어디까지 읽어야 하는지 알 수 있다.
   */
  get_filetype(filename, filetype);
  snprintf(buf, sizeof(buf),
           "HTTP/1.0 200 OK\r\n"
           "Server: Tiny Web Server\r\n"
           "Connection: close\r\n"
           "Content-length: %d\r\n"
           "Content-type: %s\r\n\r\n",
           filesize, filetype);
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n%s", buf);

  if (filesize == 0)
    return;

  /*
   * Step 2. 파일 본문을 보낸다.
   *
   * Open으로 파일 fd를 얻고, Mmap으로 파일 내용을 메모리에 올린 다음,
   * Rio_writen으로 클라이언트 소켓 fd에 그대로 쓴다.
   */
  srcfd = Open(filename, O_RDONLY, 0);
  srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);

  /*
   * mmap 이후에는 파일 내용이 srcp 주소로 보이므로 srcfd는 닫아도 된다.
   * 실제 응답 전송은 connfd인 fd로 한다는 점을 구분해서 보자.
   */
  Close(srcfd);
  Rio_writen(fd, srcp, filesize);
  Munmap(srcp, filesize);
}

/*
 * get_filetype - derive HTTP Content-Type from filename
 *
 * 브라우저는 Content-Type을 보고 본문을 어떻게 해석할지 결정한다.
 *
 * 예:
 *
 *   .html -> text/html
 *   .gif  -> image/gif
 *   .jpg  -> image/jpeg
 *   .png  -> image/png
 *
 * 구현 힌트:
 *
 *   strstr(filename, ".html")
 *   strstr(filename, ".gif")
 *   strstr(filename, ".jpg")
 *   strstr(filename, ".png")
 *
 * 그 외에는 text/plain 또는 text/html 같은 기본값을 고르면 된다.
 */
void get_filetype(char *filename, char *filetype)
{
  /*
   * filename의 확장자를 보고 HTTP Content-Type 값을 정한다.
   *
   * 이 값이 틀리면 파일 바이트는 맞게 가도 브라우저가 HTML/이미지/텍스트를
   * 엉뚱하게 해석할 수 있다.
   */
  if (strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".css"))
    strcpy(filetype, "text/css");
  else if (strstr(filename, ".js"))
    strcpy(filetype, "application/javascript");
  else
    strcpy(filetype, "text/plain");
}

/*
 * serve_dynamic - run a CGI program and send its output to the client
 *
 * 동적 컨텐츠는 서버가 파일을 그대로 보내는 것이 아니라, 프로그램을 실행한
 * 결과를 클라이언트에게 보내는 방식이다.
 *
 * 예:
 *
 *   /cgi-bin/adder?1&2
 *
 * 여기서 filename은 "./cgi-bin/adder", cgiargs는 "1&2"가 된다.
 *
 * 큰 흐름:
 *
 *   1. 클라이언트에게 먼저 기본 성공 응답 헤더를 보낸다.
 *
 *        HTTP/1.0 200 OK\r\n
 *        Server: Tiny Web Server\r\n
 *
 *      CGI 프로그램이 나머지 Content-type과 body를 출력할 수 있다.
 *
 *   2. Fork()로 자식 프로세스를 만든다.
 *
 *   3. 자식 프로세스에서:
 *        setenv("QUERY_STRING", cgiargs, 1);
 *        Dup2(fd, STDOUT_FILENO);
 *        Execve(filename, emptylist, environ);
 *
 *      핵심은 Dup2다. CGI 프로그램은 printf로 표준 출력에 쓰지만,
 *      표준 출력을 connfd로 바꿔 놓으면 그 출력이 클라이언트에게 간다.
 *
 *   4. 부모 프로세스에서:
 *        Wait(NULL);
 *
 * 구현 힌트:
 *
 *   char *emptylist[] = { NULL };
 *
 * 학습 포인트:
 *
 *   - 정적 컨텐츠는 서버가 파일을 직접 읽어서 보낸다.
 *   - 동적 컨텐츠는 서버가 프로그램을 실행하고, 그 프로그램 출력이
 *     HTTP 응답 body가 된다.
 */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
  char buf[MAXLINE];
  char *emptylist[] = {NULL};

  /*
   * Step 1. CGI 프로그램을 실행하기 전에 기본 HTTP 성공 헤더를 보낸다.
   *
   * Content-type과 Content-length는 CGI 프로그램이 직접 출력한다.
   * 그래서 여기서는 상태 라인과 서버 정보까지만 먼저 보낸다.
   */
  snprintf(buf, sizeof(buf),
           "HTTP/1.0 200 OK\r\n"
           "Server: Tiny Web Server\r\n"
           "Connection: close\r\n");
  Rio_writen(fd, buf, strlen(buf));
  printf("Response headers:\n%s", buf);

  /*
   * Step 2. 자식 프로세스가 CGI 프로그램으로 변신한다.
   *
   * 부모는 계속 Tiny 서버 코드로 남아 Wait()하고,
   * 자식은 Execve() 이후 ./cgi-bin/adder 같은 CGI 프로그램이 된다.
   */
  if (Fork() == 0)
  {
    /*
     * QUERY_STRING은 CGI 프로그램에게 인자를 넘기는 약속된 환경 변수다.
     * adder.c는 getenv("QUERY_STRING")으로 이 값을 읽는다.
     */
    setenv("QUERY_STRING", cgiargs, 1);

    /*
     * 핵심 포인트:
     *   CGI 프로그램은 printf로 STDOUT에 출력한다.
     *   STDOUT을 클라이언트 소켓 fd로 바꾸면, printf 결과가 브라우저로 간다.
     */
    Dup2(fd, STDOUT_FILENO);
    Execve(filename, emptylist, environ);
  }

  /* 부모는 자식 CGI 프로그램이 끝날 때까지 기다린다. */
  Wait(NULL);
}

/*
 * clienterror - send an HTTP error response to the client
 *
 * 서버 내부에서 에러가 났다고 그냥 printf만 하면 클라이언트는 아무것도
 * 모른다. HTTP 서버는 에러도 HTTP 응답 형식으로 보내야 한다.
 *
 * 호출 예:
 *
 *   clienterror(fd, method, "501", "Not implemented",
 *               "Tiny does not implement this method");
 *
 *   clienterror(fd, filename, "404", "Not found",
 *               "Tiny could not find this file");
 *
 * 응답 구조:
 *
 *   HTTP/1.0 404 Not found\r\n
 *   Content-type: text/html\r\n
 *   Content-length: ...\r\n
 *   \r\n
 *   <html>...</html>
 *
 * 구현 흐름:
 *
 *   1. body[MAXBUF]에 HTML 에러 페이지를 만든다.
 *   2. buf[MAXLINE]에 상태 라인과 헤더들을 만든다.
 *   3. Rio_writen(fd, buf, strlen(buf))로 헤더를 보낸다.
 *   4. Rio_writen(fd, body, strlen(body))로 본문을 보낸다.
 *
 * 주의:
 *
 *   Content-length는 body의 길이다. 헤더 길이가 아니다.
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg)
{
  char buf[MAXLINE], body[MAXBUF];

  /*
   * Step 1. 브라우저에 보여줄 HTML body를 먼저 만든다.
   *
   * Content-length 헤더를 만들려면 body 길이를 알아야 하므로 body를
   * 먼저 조립하는 순서가 자연스럽다.
   */
  snprintf(body, sizeof(body),
           "<html><title>Tiny Error</title>"
           "<body bgcolor=\"ffffff\">\r\n"
           "%s: %s\r\n"
           "<p>%s: %s\r\n"
           "<hr><em>The Tiny Web server</em>\r\n"
           "</body></html>\r\n",
           errnum, shortmsg, longmsg, cause);

  /*
   * Step 2. HTTP 에러 응답 헤더를 만든다.
   *
   * errnum/shortmsg는 상태 라인에 들어가고,
   * body 길이는 Content-length에 들어간다.
   */
  snprintf(buf, sizeof(buf),
           "HTTP/1.0 %s %s\r\n"
           "Server: Tiny Web Server\r\n"
           "Connection: close\r\n"
           "Content-type: text/html\r\n"
           "Content-length: %zu\r\n\r\n",
           errnum, shortmsg, strlen(body));

  /* Step 3. 헤더를 먼저, body를 나중에 쓴다. HTTP 응답의 기본 순서다. */
  Rio_writen(fd, buf, strlen(buf));
  Rio_writen(fd, body, strlen(body));
  printf("Error response:\n%s%s", buf, body);
}

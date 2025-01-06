/*
 * 멀티스레드 프록시 서버
 * 
 * 기능: 
 * - HTTP 프록시
 * - LRU 캐시로 성능 개선
 * - 멀티스레드로 동시 처리
 * 
 * 작성자: 김현우
 * 최종 수정일: 2024.01.07
 */

#include <stdio.h>
#include "csapp.h"
#include <semaphore.h>

/* 설정값 */
#define MAX_CACHE_SIZE 1049000  // 캐시 전체 크기 (약 1MB)
#define MAX_OBJECT_SIZE 102400  // 최대 객체 크기 (100KB)
#define MAXLINE 8192       
#define NTHREADS 4            // 쓰레드 4개면 충분할듯
#define SBUFSIZE 16          // 버퍼도 16개로 해보자

/* 브라우저 헤더 */
static const char *user_agent_hdr = 
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

/* 캐시 구조체들 */
typedef struct cache_block {
    char *uri;                  
    char *content;              
    int content_size;           
    struct cache_block *prev, *next;  
} cache_block_t;

typedef struct {
    cache_block_t *head, *tail;   
    int current_size;            
    sem_t mutex;                 
    sem_t w;                    
    int readcnt;                
} cache_t;

cache_t cache;

/* 쓰레드풀 구조체 - 세마포어로 동기화 */
typedef struct {
    int *buf;          
    int n;             
    int front;         
    int rear;          
    sem_t mutex;      
    sem_t slots;       
    sem_t items;       
} sbuf_t;

sbuf_t sbuf;

/* 캐시 초기화 - 처음에 한번만 실행 */
void cache_init() {
    cache.head = cache.tail = NULL;
    cache.current_size = 0;
    Sem_init(&cache.mutex, 0, 1);
    Sem_init(&cache.w, 0, 1);
    cache.readcnt = 0;
}

/* 캐시에서 데이터 찾기 */
cache_block_t *cache_find(char *uri) {
    P(&cache.mutex);
    cache.readcnt++;
    if (cache.readcnt == 1) 
        P(&cache.w);
    V(&cache.mutex);

    cache_block_t *block = cache.head;
    while (block) {
        if (strcmp(block->uri, uri) == 0)
            break;
        block = block->next;
    }

    P(&cache.mutex);
    cache.readcnt--;
    if (cache.readcnt == 0) 
        V(&cache.w);
    V(&cache.mutex);

    return block;
}

/* 캐시에 새 데이터 저장 */
void cache_insert(char *uri, char *content, int content_size) {
    if (content_size > MAX_OBJECT_SIZE)
        return;

    P(&cache.w);

    // 캐시 꽉 찼으면 LRU로 삭제
    while (cache.current_size + content_size > MAX_CACHE_SIZE && cache.tail) {
        cache_block_t *temp = cache.tail;
        cache.current_size -= temp->content_size;
        
        if (temp->prev)
            temp->prev->next = NULL;
        cache.tail = temp->prev;
        
        if (cache.head == temp)
            cache.head = NULL;
            
        Free(temp->uri);
        Free(temp->content);
        Free(temp);
    }

    // 새로운 데이터 넣기
    cache_block_t *new_block = Malloc(sizeof(cache_block_t));
    new_block->uri = strdup(uri);
    new_block->content = Malloc(content_size);
    memcpy(new_block->content, content, content_size);
    new_block->content_size = content_size;

    new_block->next = cache.head;
    new_block->prev = NULL;
    if (cache.head)
        cache.head->prev = new_block;
    cache.head = new_block;
    if (!cache.tail)
        cache.tail = new_block;

    cache.current_size += content_size;

    V(&cache.w);
}

/* 쓰레드풀 관련 함수들 */
void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = Calloc(n, sizeof(int)); 
    sp->n = n;                       
    sp->front = sp->rear = 0;      
    Sem_init(&sp->mutex, 0, 1);      
    Sem_init(&sp->slots, 0, n);      
    Sem_init(&sp->items, 0, 0);      
}

void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);                          
    P(&sp->mutex);                         
    sp->buf[(++sp->rear)%(sp->n)] = item;  
    V(&sp->mutex);                         
    V(&sp->items);                         
}

int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);                          
    P(&sp->mutex);                          
    item = sp->buf[(++sp->front)%(sp->n)];  
    V(&sp->mutex);                          
    V(&sp->slots);                          
    return item;
}

void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *pathname, int *port);
void build_requesthdrs(rio_t *rp, char *newreq, char *hostname, char *pathname);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

/* 요청 처리 메인 함수 */
void doit(int connfd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], pathname[MAXLINE];
    char newreq[MAXLINE];
    char port_str[MAXLINE];
    rio_t rio_client, rio_server;
    int serverfd, port;

    // 클라이언트 요청 읽기
    Rio_readinitb(&rio_client, connfd);
    Rio_readlineb(&rio_client, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);

    // GET만 지원
    if (strcasecmp(method, "GET")) {
        clienterror(connfd, method, "501", "Not Implemented",
                   "지원하지 않는 메소드입니다");
        return;
    }

    // 캐시 히트면 바로 응답
    cache_block_t *cached = cache_find(uri);
    if (cached) {
        Rio_writen(connfd, cached->content, cached->content_size);
        return;
    }

    // 캐시 미스면 서버에 요청
    parse_uri(uri, hostname, pathname, &port);
    sprintf(port_str, "%d", port);
    
    serverfd = Open_clientfd(hostname, port_str);
    if (serverfd < 0) {
        clienterror(connfd, hostname, "503", "Service Unavailable",
                   "서버 연결 실패");
        return;
    }

    Rio_readinitb(&rio_server, serverfd);
    build_requesthdrs(&rio_client, newreq, hostname, pathname);
    Rio_writen(serverfd, newreq, strlen(newreq));

    // 응답 캐싱하면서 전달
    char cache_buf[MAX_OBJECT_SIZE];
    int cache_size = 0;
    size_t n;

    while ((n = Rio_readlineb(&rio_server, buf, MAXLINE)) > 0) {
        Rio_writen(connfd, buf, n);
        
        if (cache_size + n <= MAX_OBJECT_SIZE) {
            memcpy(cache_buf + cache_size, buf, n);
            cache_size += n;
        }
    }

    if (cache_size <= MAX_OBJECT_SIZE) {
        cache_insert(uri, cache_buf, cache_size);
    }

    Close(serverfd);
}

/* URI 파싱 - 호스트/경로/포트 분리 */
void parse_uri(char *uri, char *hostname, char *pathname, int *port) {
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) == 0)
        hostbegin = uri + 7;
    else
        hostbegin = uri;
    
    hostend = strstr(hostbegin, ":");
    if (hostend) {
        len = hostend - hostbegin;
        strncpy(hostname, hostbegin, len);
        hostname[len] = '\0';
        *port = atoi(hostend + 1);
    } else {
        hostend = strstr(hostbegin, "/");
        if (!hostend) {
            strcpy(hostname, hostbegin);
            strcpy(pathname, "/");
            *port = 80;
            return;
        }
        len = hostend - hostbegin;
        strncpy(hostname, hostbegin, len);
        hostname[len] = '\0';
        *port = 80;
    }

    pathbegin = strstr(hostbegin, "/");
    if (pathbegin)
        strcpy(pathname, pathbegin);
    else
        strcpy(pathname, "/");
}

/* HTTP 요청 헤더 생성 */
void build_requesthdrs(rio_t *rp, char *newreq, char *hostname, char *pathname) {
    char buf[MAXLINE], host_hdr[MAXLINE];
    
    sprintf(newreq, "GET %s HTTP/1.0\r\n", pathname);
    
    host_hdr[0] = '\0';
    while (Rio_readlineb(rp, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) 
            break;
        
        if (strncasecmp(buf, "Host:", 5) == 0) {
            strcpy(host_hdr, buf);
            continue;
        }
        
        // 프록시 관련 헤더는 제거
        if (strncasecmp(buf, "User-Agent:", 11) == 0 ||
            strncasecmp(buf, "Connection:", 11) == 0 ||
            strncasecmp(buf, "Proxy-Connection:", 17) == 0)
            continue;
            
        strcat(newreq, buf);
    }
    
    if (!strlen(host_hdr)) 
        sprintf(host_hdr, "Host: %s\r\n", hostname);
    strcat(newreq, host_hdr);
    strcat(newreq, user_agent_hdr);
    strcat(newreq, "Connection: close\r\n");
    strcat(newreq, "Proxy-Connection: close\r\n\r\n");
}

/* 에러 응답 생성 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE];
    
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));
    
    sprintf(buf, "<html><title>프록시 에러</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>프록시 서버</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}

/* 워커 쓰레드 함수 */
void *thread(void *vargp) {
    Pthread_detach(pthread_self());
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
    return NULL;
}

/* 메인 함수 */
int main(int argc, char **argv) {
    int i, listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    
    if (argc != 2) {
        fprintf(stderr, "사용법: %s <port>\n", argv[0]);
        exit(1);
    }

    // 초기화
    cache_init();
    listenfd = Open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);

    // 워커 쓰레드 생성
    for (i = 0; i < NTHREADS; i++) 
        Pthread_create(&tid, NULL, thread, NULL);

    // 연결 수락 루프
    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = Accept(listenfd, (SA *) &clientaddr, &clientlen);
        sbuf_insert(&sbuf, connfd);
    }

    return 0;
}
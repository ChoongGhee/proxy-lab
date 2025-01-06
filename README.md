# HTTP 멀티스레드 프록시 서버

멀티스레드와 캐싱을 지원하는 HTTP 프록시 서버 구현 프로젝트입니다.

## 주요 기능

- HTTP/1.0 프록시 서버
- LRU(Least Recently Used) 캐시 구현 
- 멀티스레드 기반 동시 요청 처리
- 약 1MB 크기의 캐시로 성능 개선

## 기술 스택

- 언어: C
- 빌드: gcc
- 라이브러리: POSIX Thread, csapp.h

## 설치 및 실행

```bash
# 컴파일
gcc -o proxy proxy.c csapp.c -lpthread

# 실행 (포트 번호 지정)
./proxy <port>
```

## 테스트

```bash
# 테스트 실행
./driver.sh

# 자동으로 모든 테스트 케이스를 실행하고 결과를 보여줍니다
```

## 구현 상세

- 4개의 워커 스레드로 동시 요청 처리
- LRU 알고리즘으로 캐시 교체 정책 구현
- 세마포어 기반 스레드 동기화
- Reader-Writer 패턴으로 캐시 접근 제어

## 제한사항

- GET 메소드만 지원
- 최대 캐시 크기: 1MB
- 단일 객체 최대 크기: 100KB 
- HTTP/1.0만 지원

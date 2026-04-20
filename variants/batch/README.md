# ZTree Batch Variant

Per-zone group-commit write batching. 동일 존에 쓰려는 여러 스레드의 페이지를 모아 단일 `pwrite`로 합쳐 쓴다.

## 핵심 구조

- `zbatch_queue_t`: 존마다 하나씩 존재하는 배치 큐 (최대 `ZTREE_BATCH_MAX=4` 엔트리)
- 첫 번째 스레드가 flusher 역할을 맡고, 나머지는 대기
- flusher는 큐를 드레인하여 N개 페이지를 하나의 `pwrite(N×4KB)`로 기록
- adaptive wait: `last_batch_size > 1`이면 20us 대기하여 더 많은 엔트리 수집

## 빌드

```bash
gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
    ztree_nlt.c ztree_zone.c variants/batch/ztree_main.c \
    bench_main_ztree.c \
    -I variants/batch -I . \
    -o build/bin/ztree_batch4 -lzbd -lnvme -lpthread
```

## 실행

```bash
sudo ./build/bin/ztree_batch4 10000000 16 /dev/nvme3n2
```

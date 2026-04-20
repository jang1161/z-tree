# ZTree — CoW B+-Tree for ZNS SSDs

ZNS (Zoned Namespace) SSD의 순차 쓰기 제약에 최적화된 Copy-on-Write B+-Tree 구현.

## 주요 설계

- **Node Location Table (NLT)**: NodeID → (zone_id, slot_id) 매핑. 같은 존 내 재기록 시 NLT만 갱신하고 부모 노드 수정을 생략 (Two-Stage Tracking)
- **3-Layer 존 구조**: RLayer (메타 ping-pong), ILayer (내부노드, 라운드로빈), LLayer (리프, 핫/콜드 분리)
- **Percentile 기반 Heat 정책**: 접근 빈도 또는 최근 쓰기 시간이 중앙값 이상이면 hot 존 배치
- **Read-crab descent**: 내부노드는 rdlock, 리프에서 wrlock 업그레이드
- **Lock-free root publish**: seqlock 기반 volatile superblock, 백그라운드 체크포인트

## 폴더 구조

```
├── ztree_types.h          # 공유 타입, 페이지 레이아웃 (4KB), 상수
├── ztree_nlt.h/c          # Node Location Table
├── ztree_zone.h/c         # 존 할당기 (ILayer RR, LLayer heat-aware)
├── ztree_main.h/c         # 코어: open/close, insert, find, CoW, 캐시
├── bench_main_ztree.c     # 벤치마크 (멀티스레드 삽입)
├── watch_zone.sh          # 존 상태 실시간 모니터링
├── variants/
│   ├── batch/             # per-zone group-commit 배칭 변형
│   └── shard/             # 트리 샤딩 변형
├── paper/                 # 논문 원문 및 구현 요약
└── scripts/               # fio 테스트 스크립트
```

## 빌드

```bash
gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
    ztree_nlt.c ztree_zone.c ztree_main.c bench_main_ztree.c \
    -o build/bin/ztree -lzbd -lnvme -lpthread
```

의존성: `libzbd`, `libnvme`, `pthread`

## 실행

```bash
# 10M 키, 16스레드
sudo ./build/bin/ztree 10000000 16 /dev/nvme3n2

# 1M 키, 모든 스레드 수 (1,2,4,8,16,32,64) 자동 테스트
sudo ./build/bin/ztree 1000000 0 /dev/nvme3n2
```

## 존 모니터링

실행 중 별도 터미널에서 존 상태를 실시간으로 확인:

```bash
bash watch_zone.sh
```

RLayer/ILayer/LLayer-Hot/LLayer-Cold 별로 현재 open/closed 존과 사용률을 확인할 수 있습니다.

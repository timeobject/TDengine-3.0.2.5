/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tsdb.h"

typedef struct SMemTable          SMemTable;
typedef struct SMemData           SMemData;
typedef struct SMemSkipList       SMemSkipList;
typedef struct SMemSkipListNode   SMemSkipListNode;
typedef struct SMemSkipListCurosr SMemSkipListCurosr;

struct SMemTable {
  STsdb     *pTsdb;
  TSKEY      minKey;
  TSKEY      maxKey;
  int64_t    minVer;
  int64_t    maxVer;
  int64_t    nRows;
  int32_t    nHash;
  int32_t    nBucket;
  SMemData **pBuckets;
};

struct SMemSkipListNode {
  int8_t            level;
  SMemSkipListNode *forwards[];
};

struct SMemSkipList {
  uint32_t         seed;
  int8_t           maxLevel;
  int8_t           level;
  int32_t          size;
  SMemSkipListNode pHead[];
};

struct SMemData {
  SMemData    *pHashNext;
  tb_uid_t     suid;
  tb_uid_t     uid;
  TSKEY        minKey;
  TSKEY        maxKey;
  int64_t      minVer;
  int64_t      maxVer;
  int64_t      nRows;
  SMemSkipList sl;
};

struct SMemSkipListCurosr {
  SMemSkipList     *pSl;
  SMemSkipListNode *pNodeC;
};

#define SL_NODE_SIZE(l)        (sizeof(SMemSkipListNode) + sizeof(SMemSkipListNode *) * (l)*2)
#define SL_NODE_HALF_SIZE(l)   (sizeof(SMemSkipListNode) + sizeof(SMemSkipListNode *) * (l))
#define SL_NODE_FORWARD(n, l)  ((n)->forwards[l])
#define SL_NODE_BACKWARD(n, l) ((n)->forwards[(n)->level + (l)])
#define SL_NODE_DATA(n)        (&SL_NODE_BACKWARD(n, (n)->level))

#define SL_HEAD_NODE(sl) ((sl)->pHead)
#define SL_TAIL_NODE(sl) ((SMemSkipListNode *)&SL_NODE_FORWARD(SL_HEAD_NODE(sl), (sl)->maxLevel))

// SMemTable
int32_t tsdbMemTableCreate2(STsdb *pTsdb, SMemTable **ppMemTb) {
  SMemTable *pMemTb = NULL;

  pMemTb = taosMemoryCalloc(1, sizeof(*pMemTb));
  if (pMemTb == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  pMemTb->pTsdb = pTsdb;
  pMemTb->minKey = TSKEY_MAX;
  pMemTb->maxKey = TSKEY_MIN;
  pMemTb->minVer = -1;
  pMemTb->maxVer = -1;
  pMemTb->nRows = 0;
  pMemTb->nHash = 0;
  pMemTb->nBucket = 1024;
  pMemTb->pBuckets = taosMemoryCalloc(pMemTb->nBucket, sizeof(*pMemTb->pBuckets));
  if (pMemTb->pBuckets == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    taosMemoryFree(pMemTb);
    return -1;
  }

  *ppMemTb = pMemTb;
  return 0;
}

int32_t tsdbMemTableDestroy2(STsdb *pTsdb, SMemTable *pMemTb) {
  if (pMemTb) {
    // loop to destroy the contents (todo)
    taosMemoryFree(pMemTb->pBuckets);
    taosMemoryFree(pMemTb);
  }
  return 0;
}

int32_t tsdbInsertData2(SMemTable *pMemTb, int64_t version, const SVSubmitBlk *pSubmitBlk) {
  SMemData          *pMemData;
  STsdb             *pTsdb = pMemTb->pTsdb;
  SVnode            *pVnode = pTsdb->pVnode;
  SVBufPool         *pPool = pVnode->inUse;
  int32_t            hash;
  int32_t            tlen;
  uint8_t            buf[16];
  int32_t            rlen;
  const uint8_t     *p;
  SMemSkipListNode  *pSlNode;
  const STSRow      *pTSRow;
  SMemSkipListCurosr slc = {0};

  // search hash
  hash = (pSubmitBlk->suid + pSubmitBlk->uid) % pMemTb->nBucket;
  for (pMemData = pMemTb->pBuckets[hash]; pMemData; pMemData = pMemData->pHashNext) {
    if (pMemData->suid == pSubmitBlk->suid && pMemData->uid == pSubmitBlk->uid) break;
  }

  // create pMemData if need
  if (pMemData == NULL) {
    int8_t            maxLevel = pVnode->config.tsdbCfg.slLevel;
    int32_t           tsize = sizeof(*pMemData) + SL_NODE_HALF_SIZE(maxLevel) * 2;
    SMemSkipListNode *pHead, *pTail;

    pMemData = vnodeBufPoolMalloc(pPool, tsize);
    if (pMemData == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      return -1;
    }

    pMemData->pHashNext = NULL;
    pMemData->suid = pSubmitBlk->suid;
    pMemData->uid = pSubmitBlk->uid;
    pMemData->minKey = TSKEY_MAX;
    pMemData->maxKey = TSKEY_MIN;
    pMemData->minVer = -1;
    pMemData->maxVer = -1;
    pMemData->nRows = 0;
    pMemData->sl.seed = taosRand();
    pMemData->sl.maxLevel = maxLevel;
    pMemData->sl.level = 0;
    pMemData->sl.size = 0;
    pHead = SL_HEAD_NODE(&pMemData->sl);
    pTail = SL_TAIL_NODE(&pMemData->sl);
    pHead->level = maxLevel;
    pTail->level = maxLevel;
    for (int iLevel = 0; iLevel < maxLevel; iLevel++) {
      SL_NODE_FORWARD(pHead, iLevel) = pTail;
      SL_NODE_FORWARD(pTail, iLevel) = pHead;
    }

    // add to MemTable
    hash = (pMemData->suid + pMemData->uid) % pMemTb->nBucket;
    pMemData->pHashNext = pMemTb->pBuckets[hash];
    pMemTb->pBuckets[hash] = pMemData;
    pMemTb->nHash++;
  }

// loop to insert data to skiplist
#if 0
  tsdbMemSkipListCursorOpen(&slc, &pMemData->sl);
  p = pSubmitBlk->pData;
  for (;;) {
    if (p - (uint8_t *)pSubmitBlk->pData >= pSubmitBlk->nData) break;

    const uint8_t *pt = p;
    p = tGetBinary(p, &pTSRow, &rlen);

    // check the row (todo)

    // move the cursor to position to write (todo)
    int32_t c;
    tsdbMemSkipListCursorMoveTo(&slc, pTSRow, version, &c);
    ASSERT(c);

    // encode row
    int8_t  level = tsdbMemSkipListRandLevel(&pMemData->sl);
    int32_t tsize = SL_NODE_SIZE(level) + sizeof(version) + (p - pt);
    pSlNode = vnodeBufPoolMalloc(pPool, tsize);
    pSlNode->level = level;

    uint8_t *pData = SL_NODE_DATA(pSlNode);
    *(int64_t *)pData = version;
    pData += sizeof(version);
    memcpy(pData, pt, p - pt);

    // insert row
    tsdbMemSkipListCursorPut(&slc, pSlNode);

    // update status
    if (pTSRow->ts < pMemData->minKey) pMemData->minKey = pTSRow->ts;
    if (pTSRow->ts > pMemData->maxKey) pMemData->maxKey = pTSRow->ts;
  }
  tsdbMemSkipListCursorClose(&slc);
#endif

  if (pMemData->minVer == -1) pMemData->minVer = version;
  if (pMemData->maxVer == -1 || pMemData->maxVer < version) pMemData->maxVer = version;

  if (pMemTb->minKey < pMemData->minKey) pMemTb->minKey = pMemData->minKey;
  if (pMemTb->maxKey < pMemData->maxKey) pMemTb->maxKey = pMemData->maxKey;
  if (pMemTb->minVer == -1) pMemTb->minVer = version;
  if (pMemTb->maxVer == -1 || pMemTb->maxVer < version) pMemTb->maxVer = version;

  return 0;
}

// SMemData

// SMemSkipList
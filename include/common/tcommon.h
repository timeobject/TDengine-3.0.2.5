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

#ifndef _TD_COMMON_DEF_H_
#define _TD_COMMON_DEF_H_

#include "taosdef.h"
#include "tarray.h"
#include "tmsg.h"
#include "tvariant.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TMQ_CONF__RESET_OFFSET__LATEST = -1,
  TMQ_CONF__RESET_OFFSET__EARLIEAST = -2,
  TMQ_CONF__RESET_OFFSET__NONE = -3,
};

enum {
  TMQ_MSG_TYPE__DUMMY = 0,
  TMQ_MSG_TYPE__POLL_RSP,
  TMQ_MSG_TYPE__POLL_META_RSP,
  TMQ_MSG_TYPE__EP_RSP,
  TMQ_MSG_TYPE__END_RSP,
};

typedef enum EStreamType {
  STREAM_NORMAL = 1,
  STREAM_INVERT,
  STREAM_REPROCESS,
  STREAM_INVALID,
  STREAM_GET_ALL,
  STREAM_DELETE,
  STREAM_RETRIEVE,
} EStreamType;

typedef struct {
  SArray*   pTableList;
  SHashObj* map;  // speedup acquire the tableQueryInfo by table uid
  void*     pTagCond;
  void*     pTagIndexCond;
  uint64_t  suid;
} STableListInfo;

typedef struct SColumnDataAgg {
  int16_t colId;
  int16_t maxIndex;
  int16_t minIndex;
  int16_t numOfNull;
  int64_t sum;
  int64_t max;
  int64_t min;
} SColumnDataAgg;

typedef struct SDataBlockInfo {
  STimeWindow window;
  int32_t     rows;
  int32_t     rowSize;
  int64_t     uid;      // the uid of table, from which current data block comes
  int64_t     blockId;  // block id, generated by physical planner
  uint64_t    groupId;  // no need to serialize
  int16_t     numOfCols;
  int16_t     hasVarCol;
  int32_t     capacity;
  // TODO: optimize and remove following
  int32_t     childId;  // used for stream, do not serialize
  EStreamType type;     // used for stream, do not serialize
} SDataBlockInfo;

typedef struct SSDataBlock {
  SColumnDataAgg** pBlockAgg;
  SArray*          pDataBlock;  // SArray<SColumnInfoData>
  SDataBlockInfo   info;
} SSDataBlock;

typedef struct SVarColAttr {
  int32_t* offset;    // start position for each entry in the list
  uint32_t length;    // used buffer size that contain the valid data
  uint32_t allocLen;  // allocated buffer size
} SVarColAttr;

// pBlockAgg->numOfNull == info.rows, all data are null
// pBlockAgg->numOfNull == 0, no data are null.
typedef struct SColumnInfoData {
  SColumnInfo info;     // TODO filter info needs to be removed
  bool        hasNull;  // if current column data has null value.
  char*       pData;    // the corresponding block data in memory
  union {
    char*       nullbitmap;  // bitmap, one bit for each item in the list
    SVarColAttr varmeta;
  };
} SColumnInfoData;

typedef struct SQueryTableDataCond {
  // STimeWindow  twindow;
  uint64_t     suid;
  int32_t      order;  // desc|asc order to iterate the data block
  int32_t      numOfCols;
  SColumnInfo* colList;
  bool         loadExternalRows;  // load external rows or not
  int32_t      type;              // data block load type:
  int32_t      numOfTWindows;
  STimeWindow* twindows;
  int32_t      startVersion;
  int32_t      endVersion;
} SQueryTableDataCond;

void*   blockDataDestroy(SSDataBlock* pBlock);
int32_t tEncodeDataBlock(void** buf, const SSDataBlock* pBlock);
void*   tDecodeDataBlock(const void* buf, SSDataBlock* pBlock);

int32_t tEncodeDataBlocks(void** buf, const SArray* blocks);
void*   tDecodeDataBlocks(const void* buf, SArray** blocks);
void    colDataDestroy(SColumnInfoData* pColData);

static FORCE_INLINE void blockDestroyInner(SSDataBlock* pBlock) {
  int32_t numOfOutput = taosArrayGetSize(pBlock->pDataBlock);
  for (int32_t i = 0; i < numOfOutput; ++i) {
    SColumnInfoData* pColInfoData = (SColumnInfoData*)taosArrayGet(pBlock->pDataBlock, i);
    colDataDestroy(pColInfoData);
  }

  taosArrayDestroy(pBlock->pDataBlock);
  taosMemoryFreeClear(pBlock->pBlockAgg);
}

static FORCE_INLINE void tDeleteSSDataBlock(SSDataBlock* pBlock) { blockDestroyInner(pBlock); }

//======================================================================================================================
// the following structure shared by parser and executor
typedef struct SColumn {
  union {
    uint64_t uid;
    int64_t  dataBlockId;
  };

  int16_t colId;
  int16_t slotId;

  char    name[TSDB_COL_NAME_LEN];
  int8_t  flag;  // column type: normal column, tag, or user-input column (integer/float/string)
  int16_t type;
  int32_t bytes;
  uint8_t precision;
  uint8_t scale;
} SColumn;

typedef struct STableBlockDistInfo {
  uint32_t rowSize;
  uint16_t numOfFiles;
  uint32_t numOfTables;
  uint32_t numOfBlocks;
  uint64_t totalSize;
  uint64_t totalRows;
  int32_t  maxRows;
  int32_t  minRows;
  int32_t  defMinRows;
  int32_t  defMaxRows;
  int32_t  firstSeekTimeUs;
  uint32_t numOfInmemRows;
  uint32_t numOfSmallBlocks;
  int32_t  blockRowsHisto[20];
} STableBlockDistInfo;

int32_t tSerializeBlockDistInfo(void* buf, int32_t bufLen, const STableBlockDistInfo* pInfo);
int32_t tDeserializeBlockDistInfo(void* buf, int32_t bufLen, STableBlockDistInfo* pInfo);

enum {
  FUNC_PARAM_TYPE_VALUE = 0x1,
  FUNC_PARAM_TYPE_COLUMN = 0x2,
};

typedef struct SFunctParam {
  int32_t  type;
  SColumn* pCol;
  SVariant param;
} SFunctParam;

// the structure for sql function in select clause
typedef struct SResSchame {
  int8_t  type;
  int32_t slotId;
  int32_t bytes;
  int32_t precision;
  int32_t scale;
  char    name[TSDB_COL_NAME_LEN];
} SResSchema;

typedef struct SExprBasicInfo {
  SResSchema   resSchema;
  int16_t      numOfParams;  // argument value of each function
  SFunctParam* pParam;
} SExprBasicInfo;

typedef struct SExprInfo {
  struct SExprBasicInfo base;
  struct tExprNode*     pExpr;
} SExprInfo;

typedef struct {
  const char* key;
  int32_t     keyLen;
  uint8_t     type;
  union {
    const char* value;
    int64_t     i;
    uint64_t    u;
    double      d;
    float       f;
  };
  int32_t length;
} SSmlKv;

#define QUERY_ASC_FORWARD_STEP  1
#define QUERY_DESC_FORWARD_STEP -1

#define GET_FORWARD_DIRECTION_FACTOR(ord) (((ord) == TSDB_ORDER_ASC) ? QUERY_ASC_FORWARD_STEP : QUERY_DESC_FORWARD_STEP)

#define SORT_QSORT_T              0x1
#define SORT_SPILLED_MERGE_SORT_T 0x2
typedef struct SSortExecInfo {
  int32_t sortMethod;
  int32_t sortBuffer;
  int32_t loops;       // loop count
  int32_t writeBytes;  // write io bytes
  int32_t readBytes;   // read io bytes
} SSortExecInfo;

#ifdef __cplusplus
}
#endif

#endif /*_TD_COMMON_DEF_H_*/

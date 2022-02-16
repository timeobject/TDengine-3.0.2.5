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

#define _DEFAULT_SOURCE
#include "mndTopic.h"
#include "mndDb.h"
#include "mndDnode.h"
#include "mndMnode.h"
#include "mndShow.h"
#include "mndStb.h"
#include "mndTrans.h"
#include "mndUser.h"
#include "mndVgroup.h"
#include "tname.h"

#define MND_TOPIC_VER_NUMBER 1
#define MND_TOPIC_RESERVE_SIZE 64

static int32_t mndTopicActionInsert(SSdb *pSdb, SMqTopicObj *pTopic);
static int32_t mndTopicActionDelete(SSdb *pSdb, SMqTopicObj *pTopic);
static int32_t mndTopicActionUpdate(SSdb *pSdb, SMqTopicObj *pTopic, SMqTopicObj *pNewTopic);
static int32_t mndProcessCreateTopicReq(SMnodeMsg *pReq);
static int32_t mndProcessDropTopicReq(SMnodeMsg *pReq);
static int32_t mndProcessDropTopicInRsp(SMnodeMsg *pRsp);
static int32_t mndProcessTopicMetaReq(SMnodeMsg *pReq);
static int32_t mndGetTopicMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta);
static int32_t mndRetrieveTopic(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows);
static void    mndCancelGetNextTopic(SMnode *pMnode, void *pIter);

int32_t mndInitTopic(SMnode *pMnode) {
  SSdbTable table = {.sdbType = SDB_TOPIC,
                     .keyType = SDB_KEY_BINARY,
                     .encodeFp = (SdbEncodeFp)mndTopicActionEncode,
                     .decodeFp = (SdbDecodeFp)mndTopicActionDecode,
                     .insertFp = (SdbInsertFp)mndTopicActionInsert,
                     .updateFp = (SdbUpdateFp)mndTopicActionUpdate,
                     .deleteFp = (SdbDeleteFp)mndTopicActionDelete};

  mndSetMsgHandle(pMnode, TDMT_MND_CREATE_TOPIC, mndProcessCreateTopicReq);
  mndSetMsgHandle(pMnode, TDMT_MND_DROP_TOPIC, mndProcessDropTopicReq);
  mndSetMsgHandle(pMnode, TDMT_VND_DROP_TOPIC_RSP, mndProcessDropTopicInRsp);

  return sdbSetTable(pMnode->pSdb, table);
}

void mndCleanupTopic(SMnode *pMnode) {}

SSdbRaw *mndTopicActionEncode(SMqTopicObj *pTopic) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;

  int32_t  logicalPlanLen = strlen(pTopic->logicalPlan) + 1;
  int32_t  physicalPlanLen = strlen(pTopic->physicalPlan) + 1;
  int32_t  size = sizeof(SMqTopicObj) + logicalPlanLen + physicalPlanLen + pTopic->sqlLen + MND_TOPIC_RESERVE_SIZE;
  SSdbRaw *pRaw = sdbAllocRaw(SDB_TOPIC, MND_TOPIC_VER_NUMBER, size);
  if (pRaw == NULL) goto TOPIC_ENCODE_OVER;

  int32_t dataPos = 0;
  SDB_SET_BINARY(pRaw, dataPos, pTopic->name, TSDB_TABLE_FNAME_LEN, TOPIC_ENCODE_OVER);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->db, TSDB_DB_FNAME_LEN, TOPIC_ENCODE_OVER);
  SDB_SET_INT64(pRaw, dataPos, pTopic->createTime, TOPIC_ENCODE_OVER);
  SDB_SET_INT64(pRaw, dataPos, pTopic->updateTime, TOPIC_ENCODE_OVER);
  SDB_SET_INT64(pRaw, dataPos, pTopic->uid, TOPIC_ENCODE_OVER);
  SDB_SET_INT64(pRaw, dataPos, pTopic->dbUid, TOPIC_ENCODE_OVER);
  SDB_SET_INT32(pRaw, dataPos, pTopic->version, TOPIC_ENCODE_OVER);
  SDB_SET_INT32(pRaw, dataPos, pTopic->sqlLen, TOPIC_ENCODE_OVER);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->sql, pTopic->sqlLen, TOPIC_ENCODE_OVER);
  SDB_SET_INT32(pRaw, dataPos, logicalPlanLen, TOPIC_ENCODE_OVER);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->logicalPlan, logicalPlanLen, TOPIC_ENCODE_OVER);

  SDB_SET_INT32(pRaw, dataPos, physicalPlanLen, TOPIC_ENCODE_OVER);
  SDB_SET_BINARY(pRaw, dataPos, pTopic->physicalPlan, physicalPlanLen, TOPIC_ENCODE_OVER);

  SDB_SET_RESERVE(pRaw, dataPos, MND_TOPIC_RESERVE_SIZE, TOPIC_ENCODE_OVER);
  SDB_SET_DATALEN(pRaw, dataPos, TOPIC_ENCODE_OVER);

  terrno = TSDB_CODE_SUCCESS;

TOPIC_ENCODE_OVER:
  if (terrno != TSDB_CODE_SUCCESS) {
    mError("topic:%s, failed to encode to raw:%p since %s", pTopic->name, pRaw, terrstr());
    sdbFreeRaw(pRaw);
    return NULL;
  }

  mTrace("topic:%s, encode to raw:%p, row:%p", pTopic->name, pRaw, pTopic);
  return pRaw;
}

SSdbRow *mndTopicActionDecode(SSdbRaw *pRaw) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;
  int8_t sver = 0;
  if (sdbGetRawSoftVer(pRaw, &sver) != 0) goto TOPIC_DECODE_OVER;

  if (sver != MND_TOPIC_VER_NUMBER) {
    terrno = TSDB_CODE_SDB_INVALID_DATA_VER;
    goto TOPIC_DECODE_OVER;
  }

  int32_t  size = sizeof(SMqTopicObj);
  SSdbRow *pRow = sdbAllocRow(size);
  if (pRow == NULL) goto TOPIC_DECODE_OVER;

  SMqTopicObj *pTopic = sdbGetRowObj(pRow);
  if (pTopic == NULL) goto TOPIC_DECODE_OVER;

  int32_t len;
  int32_t dataPos = 0;
  SDB_GET_BINARY(pRaw, dataPos, pTopic->name, TSDB_TABLE_FNAME_LEN, TOPIC_DECODE_OVER);
  SDB_GET_BINARY(pRaw, dataPos, pTopic->db, TSDB_DB_FNAME_LEN, TOPIC_DECODE_OVER);
  SDB_GET_INT64(pRaw, dataPos, &pTopic->createTime, TOPIC_DECODE_OVER);
  SDB_GET_INT64(pRaw, dataPos, &pTopic->updateTime, TOPIC_DECODE_OVER);
  SDB_GET_INT64(pRaw, dataPos, &pTopic->uid, TOPIC_DECODE_OVER);
  SDB_GET_INT64(pRaw, dataPos, &pTopic->dbUid, TOPIC_DECODE_OVER);
  SDB_GET_INT32(pRaw, dataPos, &pTopic->version, TOPIC_DECODE_OVER);
  SDB_GET_INT32(pRaw, dataPos, &pTopic->sqlLen, TOPIC_DECODE_OVER);

  pTopic->sql = calloc(pTopic->sqlLen + 1, sizeof(char));
  SDB_GET_BINARY(pRaw, dataPos, pTopic->sql, pTopic->sqlLen, TOPIC_DECODE_OVER);

  SDB_GET_INT32(pRaw, dataPos, &len, TOPIC_DECODE_OVER);
  pTopic->logicalPlan = calloc(len + 1, sizeof(char));
  if (pTopic->logicalPlan == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto TOPIC_DECODE_OVER;
  }
  SDB_GET_BINARY(pRaw, dataPos, pTopic->logicalPlan, len, TOPIC_DECODE_OVER);

  SDB_GET_INT32(pRaw, dataPos, &len, TOPIC_DECODE_OVER);
  pTopic->physicalPlan = calloc(len + 1, sizeof(char));
  if (pTopic->physicalPlan == NULL) {
    free(pTopic->logicalPlan);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto TOPIC_DECODE_OVER;
  }
  SDB_GET_BINARY(pRaw, dataPos, pTopic->physicalPlan, len, TOPIC_DECODE_OVER);

  SDB_GET_RESERVE(pRaw, dataPos, MND_TOPIC_RESERVE_SIZE, TOPIC_DECODE_OVER);

  terrno = TSDB_CODE_SUCCESS;

TOPIC_DECODE_OVER:
  if (terrno != TSDB_CODE_SUCCESS) {
    mError("topic:%s, failed to decode from raw:%p since %s", pTopic->name, pRaw, terrstr());
    tfree(pRow);
    return NULL;
  }

  mTrace("topic:%s, decode from raw:%p, row:%p", pTopic->name, pRaw, pTopic);
  return pRow;
}

static int32_t mndTopicActionInsert(SSdb *pSdb, SMqTopicObj *pTopic) {
  mTrace("topic:%s, perform insert action", pTopic->name);
  return 0;
}

static int32_t mndTopicActionDelete(SSdb *pSdb, SMqTopicObj *pTopic) {
  mTrace("topic:%s, perform delete action", pTopic->name);
  return 0;
}

static int32_t mndTopicActionUpdate(SSdb *pSdb, SMqTopicObj *pOldTopic, SMqTopicObj *pNewTopic) {
  mTrace("topic:%s, perform update action", pOldTopic->name);
  atomic_exchange_32(&pOldTopic->updateTime, pNewTopic->updateTime);
  atomic_exchange_32(&pOldTopic->version, pNewTopic->version);

  taosWLockLatch(&pOldTopic->lock);

  // TODO handle update

  taosWUnLockLatch(&pOldTopic->lock);
  return 0;
}

SMqTopicObj *mndAcquireTopic(SMnode *pMnode, char *topicName) {
  SSdb        *pSdb = pMnode->pSdb;
  SMqTopicObj *pTopic = sdbAcquire(pSdb, SDB_TOPIC, topicName);
  if (pTopic == NULL) {
    terrno = TSDB_CODE_MND_TOPIC_NOT_EXIST;
  }
  return pTopic;
}

void mndReleaseTopic(SMnode *pMnode, SMqTopicObj *pTopic) {
  SSdb *pSdb = pMnode->pSdb;
  sdbRelease(pSdb, pTopic);
}

static SDbObj *mndAcquireDbByTopic(SMnode *pMnode, char *topicName) {
  SName name = {0};
  tNameFromString(&name, topicName, T_NAME_ACCT | T_NAME_DB | T_NAME_TABLE);

  char db[TSDB_TABLE_FNAME_LEN] = {0};
  tNameGetFullDbName(&name, db);

  return mndAcquireDb(pMnode, db);
}

static SDDropTopicReq *mndBuildDropTopicMsg(SMnode *pMnode, SVgObj *pVgroup, SMqTopicObj *pTopic) {
  int32_t contLen = sizeof(SDDropTopicReq);

  SDDropTopicReq *pDrop = calloc(1, contLen);
  if (pDrop == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pDrop->head.contLen = htonl(contLen);
  pDrop->head.vgId = htonl(pVgroup->vgId);
  memcpy(pDrop->name, pTopic->name, TSDB_TABLE_FNAME_LEN);
  pDrop->tuid = htobe64(pTopic->uid);

  return pDrop;
}

static int32_t mndCheckCreateTopicMsg(SMCreateTopicReq *creattopReq) {
  // deserialize and other stuff
  return 0;
}

static int32_t mndCreateTopic(SMnode *pMnode, SMnodeMsg *pReq, SMCreateTopicReq *pCreate, SDbObj *pDb) {
  mDebug("topic:%s to create", pCreate->name);
  SMqTopicObj topicObj = {0};
  tstrncpy(topicObj.name, pCreate->name, TSDB_TOPIC_FNAME_LEN);
  tstrncpy(topicObj.db, pDb->name, TSDB_DB_FNAME_LEN);
  topicObj.createTime = taosGetTimestampMs();
  topicObj.updateTime = topicObj.createTime;
  topicObj.uid = mndGenerateUid(pCreate->name, strlen(pCreate->name));
  topicObj.dbUid = pDb->uid;
  topicObj.version = 1;
  topicObj.sql = pCreate->sql;
  topicObj.physicalPlan = pCreate->physicalPlan;
  topicObj.logicalPlan = pCreate->logicalPlan;
  topicObj.sqlLen = strlen(pCreate->sql);

  SSdbRaw *pTopicRaw = mndTopicActionEncode(&topicObj);
  if (pTopicRaw == NULL) return -1;
  if (sdbSetRawStatus(pTopicRaw, SDB_STATUS_READY) != 0) return -1;
  /*STrans *pTrans = mndTransCreate(pMnode, TRN_POLICY_RETRY, &pReq->rpcMsg);*/
  /*mndTransAppendRedolog(pTrans, pTopicRaw);*/
  /*if (mndTransPrepare(pMnode, pTrans) != 0) {*/
  /*mError("mq-createTopic-trans:%d, failed to prepare since %s", pTrans->id, terrstr());*/
  /*mndTransDrop(pTrans);*/
  /*return -1;*/
  /*}*/
  /*mndTransDrop(pTrans);*/
  /*return 0;*/
  return sdbWrite(pMnode->pSdb, pTopicRaw);
}

static int32_t mndProcessCreateTopicReq(SMnodeMsg *pReq) {
  SMnode *pMnode = pReq->pMnode;
  char   *msgStr = pReq->rpcMsg.pCont;

  SMCreateTopicReq createTopicReq = {0};
  tDeserializeSMCreateTopicReq(msgStr, &createTopicReq);

  mDebug("topic:%s, start to create, sql:%s", createTopicReq.name, createTopicReq.sql);

  if (mndCheckCreateTopicMsg(&createTopicReq) != 0) {
    mError("topic:%s, failed to create since %s", createTopicReq.name, terrstr());
    return -1;
  }

  SMqTopicObj *pTopic = mndAcquireTopic(pMnode, createTopicReq.name);
  if (pTopic != NULL) {
    sdbRelease(pMnode->pSdb, pTopic);
    if (createTopicReq.igExists) {
      mDebug("topic:%s, already exist, ignore exist is set", createTopicReq.name);
      return 0;
    } else {
      terrno = TSDB_CODE_MND_TOPIC_ALREADY_EXIST;
      mError("topic:%s, failed to create since already exists", createTopicReq.name);
      return -1;
    }
  }

  SDbObj *pDb = mndAcquireDbByTopic(pMnode, createTopicReq.name);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    mError("topic:%s, failed to create since %s", createTopicReq.name, terrstr());
    return -1;
  }

  int32_t code = mndCreateTopic(pMnode, pReq, &createTopicReq, pDb);
  mndReleaseDb(pMnode, pDb);

  if (code != TSDB_CODE_SUCCESS) {
    terrno = code;
    mError("topic:%s, failed to create since %s", createTopicReq.name, terrstr());
    return -1;
  }

  return 0;
}

static int32_t mndDropTopic(SMnode *pMnode, SMnodeMsg *pReq, SMqTopicObj *pTopic) { return 0; }

static int32_t mndProcessDropTopicReq(SMnodeMsg *pReq) {
  SMnode        *pMnode = pReq->pMnode;
  SMDropTopicReq dropReq = {0};

  if (tDeserializeSMDropTopicReq(pReq->rpcMsg.pCont, pReq->rpcMsg.contLen, &dropReq) != 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    return -1;
  }

  mDebug("topic:%s, start to drop", dropReq.name);

  SMqTopicObj *pTopic = mndAcquireTopic(pMnode, dropReq.name);
  if (pTopic == NULL) {
    if (dropReq.igNotExists) {
      mDebug("topic:%s, not exist, ignore not exist is set", dropReq.name);
      return 0;
    } else {
      terrno = TSDB_CODE_MND_TOPIC_NOT_EXIST;
      mError("topic:%s, failed to drop since %s", dropReq.name, terrstr());
      return -1;
    }
  }

  int32_t code = mndDropTopic(pMnode, pReq, pTopic);
  mndReleaseTopic(pMnode, pTopic);

  if (code != 0) {
    terrno = code;
    mError("topic:%s, failed to drop since %s", dropReq.name, terrstr());
    return -1;
  }

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

static int32_t mndProcessDropTopicInRsp(SMnodeMsg *pRsp) {
  mndTransProcessRsp(pRsp);
  return 0;
}

static int32_t mndProcessTopicMetaReq(SMnodeMsg *pReq) {
  SMnode       *pMnode = pReq->pMnode;
  STableInfoReq infoReq = {0};

  if (tSerializeSTableInfoReq(pReq->rpcMsg.pCont, pReq->rpcMsg.contLen, &infoReq) != 0) {
    terrno = TSDB_CODE_INVALID_MSG;
    return -1;
  }

  mDebug("topic:%s, start to retrieve meta", infoReq.tbName);

#if 0
  SDbObj *pDb = mndAcquireDbByTopic(pMnode, pInfo->tableFname);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    mError("topic:%s, failed to retrieve meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  STopicObj *pTopic = mndAcquireTopic(pMnode, pInfo->tableFname);
  if (pTopic == NULL) {
    mndReleaseDb(pMnode, pDb);
    terrno = TSDB_CODE_MND_INVALID_TOPIC;
    mError("topic:%s, failed to get meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  taosRLockLatch(&pTopic->lock);
  int32_t totalCols = pTopic->numOfColumns + pTopic->numOfTags;
  int32_t contLen = sizeof(STableMetaRsp) + totalCols * sizeof(SSchema);

  STableMetaRsp *pMeta = rpcMallocCont(contLen);
  if (pMeta == NULL) {
    taosRUnLockLatch(&pTopic->lock);
    mndReleaseDb(pMnode, pDb);
    mndReleaseTopic(pMnode, pTopic);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    mError("topic:%s, failed to get meta since %s", pInfo->tableFname, terrstr());
    return -1;
  }

  memcpy(pMeta->topicFname, pTopic->name, TSDB_TABLE_FNAME_LEN);
  pMeta->numOfTags = htonl(pTopic->numOfTags);
  pMeta->numOfColumns = htonl(pTopic->numOfColumns);
  pMeta->precision = pDb->cfg.precision;
  pMeta->tableType = TSDB_SUPER_TABLE;
  pMeta->update = pDb->cfg.update;
  pMeta->sversion = htonl(pTopic->version);
  pMeta->tuid = htonl(pTopic->uid);

  for (int32_t i = 0; i < totalCols; ++i) {
    SSchema *pSchema = &pMeta->pSchemas[i];
    SSchema *pSrcSchema = &pTopic->pSchema[i];
    memcpy(pSchema->name, pSrcSchema->name, TSDB_COL_NAME_LEN);
    pSchema->type = pSrcSchema->type;
    pSchema->colId = htonl(pSrcSchema->colId);
    pSchema->bytes = htonl(pSrcSchema->bytes);
  }
  taosRUnLockLatch(&pTopic->lock);
  mndReleaseDb(pMnode, pDb);
  mndReleaseTopic(pMnode, pTopic);

  pReq->pCont = pMeta;
  pReq->contLen = contLen;

  mDebug("topic:%s, meta is retrieved, cols:%d tags:%d", pInfo->tableFname, pTopic->numOfColumns, pTopic->numOfTags);
#endif
  return 0;
}

static int32_t mndGetNumOfTopics(SMnode *pMnode, char *dbName, int32_t *pNumOfTopics) {
  SSdb *pSdb = pMnode->pSdb;

  SDbObj *pDb = mndAcquireDb(pMnode, dbName);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    return -1;
  }

  int32_t numOfTopics = 0;
  void   *pIter = NULL;
  while (1) {
    SMqTopicObj *pTopic = NULL;
    pIter = sdbFetch(pSdb, SDB_TOPIC, pIter, (void **)&pTopic);
    if (pIter == NULL) break;

    numOfTopics++;

    sdbRelease(pSdb, pTopic);
  }

  *pNumOfTopics = numOfTopics;
  return 0;
}

static int32_t mndGetTopicMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta) {
  SMnode *pMnode = pReq->pMnode;
  SSdb   *pSdb = pMnode->pSdb;

  if (mndGetNumOfTopics(pMnode, pShow->db, &pShow->numOfRows) != 0) {
    return -1;
  }

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->pSchemas;

  pShow->bytes[cols] = TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "name");
  pSchema[cols].bytes = pShow->bytes[cols];
  cols++;

  pShow->bytes[cols] = 8;
  pSchema[cols].type = TSDB_DATA_TYPE_TIMESTAMP;
  strcpy(pSchema[cols].name, "create_time");
  pSchema[cols].bytes = pShow->bytes[cols];
  cols++;

  pMeta->numOfColumns = cols;
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->numOfRows = sdbGetSize(pSdb, SDB_TOPIC);
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];
  strcpy(pMeta->tbName, mndShowStr(pShow->type));

  return 0;
}

static int32_t mndRetrieveTopic(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows) {
  SMnode      *pMnode = pReq->pMnode;
  SSdb        *pSdb = pMnode->pSdb;
  int32_t      numOfRows = 0;
  SMqTopicObj *pTopic = NULL;
  int32_t      cols = 0;
  char        *pWrite;
  char         prefix[TSDB_DB_FNAME_LEN] = {0};

  SDbObj *pDb = mndAcquireDb(pMnode, pShow->db);
  if (pDb == NULL) return 0;

  tstrncpy(prefix, pShow->db, TSDB_DB_FNAME_LEN);
  strcat(prefix, TS_PATH_DELIMITER);
  int32_t prefixLen = (int32_t)strlen(prefix);

  while (numOfRows < rows) {
    pShow->pIter = sdbFetch(pSdb, SDB_TOPIC, pShow->pIter, (void **)&pTopic);
    if (pShow->pIter == NULL) break;

    if (pTopic->dbUid != pDb->uid) {
      if (strncmp(pTopic->name, prefix, prefixLen) != 0) {
        mError("Inconsistent table data, name:%s, db:%s, dbUid:%" PRIu64, pTopic->name, pDb->name, pDb->uid);
      }

      sdbRelease(pSdb, pTopic);
      continue;
    }

    cols = 0;

    char topicName[TSDB_TABLE_NAME_LEN] = {0};
    tstrncpy(topicName, pTopic->name + prefixLen, TSDB_TABLE_NAME_LEN);
    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    STR_TO_VARSTR(pWrite, topicName);
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int64_t *)pWrite = pTopic->createTime;
    cols++;

    numOfRows++;
    sdbRelease(pSdb, pTopic);
  }

  mndReleaseDb(pMnode, pDb);
  pShow->numOfReads += numOfRows;
  mndVacuumResult(data, pShow->numOfColumns, numOfRows, rows, pShow);
  return numOfRows;
}

static void mndCancelGetNextTopic(SMnode *pMnode, void *pIter) {
  SSdb *pSdb = pMnode->pSdb;
  sdbCancelFetch(pSdb, pIter);
}

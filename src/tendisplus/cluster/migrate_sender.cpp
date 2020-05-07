#include "glog/logging.h"
#include "tendisplus/replication/repl_util.h"
#include "tendisplus/cluster/migrate_sender.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/utils/invariant.h"
namespace tendisplus {

ChunkMigrateSender::ChunkMigrateSender(const std::bitset<CLUSTER_SLOTS>& slots,
    std::shared_ptr<ServerEntry> svr,
    std::shared_ptr<ServerParams> cfg) :
    _slots(slots),
    _svr(svr),
    _cfg(cfg),
    _sendstate(MigrateSenderStatus::SNAPSHOT_BEGIN),
    _consistency(false),
    _curBinlogid(UINT64_MAX) {
        _clusterState = _svr->getClusterMgr()->getClusterState();

}


Status ChunkMigrateSender::sendChunk() {
    LOG(INFO) <<"sendChunk begin on store:" << _storeid;
    Status s = sendSnapshot(_slots);
    if (!s.ok()) {
        return s;
    }
    LOG(INFO) <<"send snapshot finish on store:" << _storeid;
    // define max time to catch up binlog
    _sendstate = MigrateSenderStatus::SNAPSHOT_DONE;

    uint16_t  maxTime = 10;

    s = sendBinlog(maxTime);
    if (!s.ok()) {
        LOG(ERROR) << "catch up binlog fail on storeid:" << _storeid;
        return  s;
    }
    _sendstate = MigrateSenderStatus::BINLOG_DONE;
    LOG(INFO) << "send binlog finish on store:" << _storeid;
    s = sendOver();

    if (!s.ok()) {
        LOG(ERROR) << "sendover error";
        if (s.code() == ErrorCodes::ERR_CLUSTER) {
            LOG(ERROR) << "sendover error cluster";
            return {ErrorCodes::ERR_CLUSTER, "send over fail on"};
        }
        return s;
    }

    /* unlock after receive package */
    s = _svr->getMigrateManager()->unlockChunks(_slots);
    if (!s.ok()) {
        LOG(ERROR) << "unlock fail on slots:"+ _slots.to_string();
        return  {ErrorCodes::ERR_CLUSTER, "unlock fail on slots"};
    }

    _sendstate = MigrateSenderStatus::METACHANGE_DONE;
    LOG(INFO) <<"sendChunk end on store:" << _storeid;

    return{ ErrorCodes::ERR_OK, "" };
}



void ChunkMigrateSender::setDstNode(const std::string nodeid) {
    _nodeid = nodeid;
    _dstNode = _clusterState->clusterLookupNode(_nodeid);
}

void ChunkMigrateSender::setSenderStatus(MigrateSenderStatus s) {
    _sendstate  = s;
}

// check if bitmap all belong to dst node
bool ChunkMigrateSender::checkSlotsBlongDst(const std::bitset<CLUSTER_SLOTS>& slots) {
    for (size_t id =0; id < slots.size(); id++) {
        if (slots.test(id)) {
            if (_clusterState->getNodeBySlot(id) != _dstNode) {
                LOG(WARNING) << "slot:" << id << "not belong to:" << _nodeid;
                return false;
             }
        }
    }
    return true;
}

Expected<Transaction*> ChunkMigrateSender::initTxn() {
    auto kvstore = _dbWithLock->store;
    auto ptxn = kvstore->createTransaction(NULL);

    if (!ptxn.ok()) {
        return ptxn.status();
    }
    // snapShot open
    Transaction* txn = ptxn.value().release();
    txn->SetSnapshot();
    LOG(INFO) << "initTxn SetSnapshot";
    return  txn;
}

Expected<uint64_t> ChunkMigrateSender::sendRange(Transaction* txn,
                                uint32_t begin, uint32_t end) {
    LOG(INFO) << "snapshot sendRange begin, beginSlot:" << begin
              << " endSlot:" << end;
    auto cursor = std::move(txn->createSlotsCursor(begin, end));
    uint32_t totalWriteNum = 0;
    uint32_t curWriteNum = 0;
    uint32_t curWriteLen = 0;
    uint32_t timeoutSec = 100;
    Status s;
    while (true) {
        Expected<Record> expRcd = cursor->next();
        if (expRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
                LOG(INFO) << "snapshot sendRange Record is over, totalWriteNum:"
                          << totalWriteNum
                          << " storeid:" << _storeid;
            break;
        }

        if (!expRcd.ok()) {
            return expRcd.status();
        }
        Record &rcd = expRcd.value();
        const RecordKey &rcdKey = rcd.getRecordKey();

        std::string key = rcdKey.encode();
        const RecordValue& rcdValue = rcd.getRecordValue();
        std::string value = rcdValue.encode();

        SyncWriteData("0");

        uint32_t keylen = key.size();
        SyncWriteData(string((char*)&keylen, sizeof(uint32_t)));

        SyncWriteData(key);

        uint32_t valuelen = value.size();
        SyncWriteData(string((char*)&valuelen, sizeof(uint32_t)));
        SyncWriteData(value);

        curWriteNum++;
        totalWriteNum++;
        curWriteLen+= 1 + sizeof(uint32_t) + keylen + sizeof(uint32_t) + valuelen;

        if (curWriteNum >= 1000) {
            SyncWriteData("1");
            SyncReadData(exptData, 3, timeoutSec)
            if (exptData.value() != "+OK") {
                LOG(ERROR) << "read data is not +OK. totalWriteNum:" << totalWriteNum
                           << " curWriteNum:" << curWriteNum
                           << " data:" << exptData.value();
                return { ErrorCodes::ERR_INTERNAL, "read +OK failed"};
            }
            curWriteNum = 0;
            curWriteLen = 0;
        }
    }
    //send over of one slot
    SyncWriteData("2");
    SyncReadData(exptData, 3, timeoutSec)

    if (exptData.value() != "+OK") {
        LOG(ERROR) << "read receiver data is not +OK on slot:" << begin;
        return { ErrorCodes::ERR_INTERNAL, "read +OK failed"};
    }
    LOG(INFO) << "snapshot sendRange end, storeid:" << _storeid
        << " beginSlot:" << begin
        << " endSlot:" << end
        << " totalKeynum:" << totalWriteNum;

    return totalWriteNum;
}

// deal with slots that is not continuous
Status ChunkMigrateSender::sendSnapshot(const SlotsBitmap& slots) {
    Status s;
    auto expdb = _svr->getSegmentMgr()->getDb(NULL, _storeid,
                                              mgl::LockMode::LOCK_IS);
    if (!expdb.ok()) {
        return expdb.status();
    }
    _dbWithLock = std::make_unique<DbWithLock>(std::move(expdb.value()));
    auto kvstore = _dbWithLock->store;

    _curBinlogid = kvstore->getHighestBinlogId();

    auto eTxn = initTxn();
    if (!eTxn.ok()) {
        return eTxn.status();
    }
    Transaction * txn = eTxn.value();
    uint32_t timeoutSec = 160;
    uint32_t sendSlotNum = 0;
    for (size_t  i = 0; i < CLUSTER_SLOTS; i++) {
        if (slots.test(i)) {
            sendSlotNum++;
            auto ret = sendRange(txn, i , i+1);
            if (!ret.ok()) {
                LOG(ERROR) << "sendRange failed, slot:" << i << "-" << i + 1;
                return ret.status();
            }
            _snapshotKeyNum += ret.value();
        }

    }
    SyncWriteData("3");  // send over of all
    SyncReadData(exptData, 3, timeoutSec)
    if (exptData.value() != "+OK") {
        LOG(ERROR) << "read receiver data is not +OK, data:"
                   << exptData.value();
        return { ErrorCodes::ERR_INTERNAL, "read +OK failed"};
    }
    LOG(INFO) << "sendSnapshot finished, storeid:" << _storeid
              <<" sendSlotNum:" << sendSlotNum
              <<" totalWriteNum:" << _snapshotKeyNum;
    return  {ErrorCodes::ERR_OK, "finish snapshot of bitmap"};
}


uint64_t ChunkMigrateSender::getMaxBinLog(Transaction* ptxn) {
    uint64_t maxBinlogId;
    auto expBinlogidMax = RepllogCursorV2::getMaxBinlogId(ptxn);
    if (expBinlogidMax.status().code() == ErrorCodes::ERR_EXHAUST) {
        maxBinlogId = 0;
    } else {
        maxBinlogId = expBinlogidMax.value();
    }
    return  maxBinlogId;
}

// catch up binlog from start to end
Expected<uint64_t> ChunkMigrateSender::catchupBinlog(uint64_t start, uint64_t end,
                                            const std::bitset<CLUSTER_SLOTS> &slots) {
    bool needHeartbeat = false;
    auto s =  SendSlotsBinlog(_client.get(), _storeid, _dstStoreid,
            start, end , needHeartbeat, slots,  _svr, _cfg);
    if (!s.ok()) {
        LOG(ERROR) << "ChunkMigrateSender::sendBinlog to client:"
                   << _client->getRemoteRepr() << " failed:"
                   << s.status().toString();
    }
    return  s.value();
}

// catch up for maxTime
bool ChunkMigrateSender::pursueBinLog(uint16_t  maxTime , uint64_t  &startBinLog ,
                                        uint64_t &binlogHigh, Transaction *txn) {
    uint32_t  distance =  _svr->getParams()->migrateDistance;
    uint64_t maxBinlogId = 0;
    bool finishCatchup = true;
    PStore kvstore = _dbWithLock->store;
    uint32_t  catchupTimes = 0;
    while (catchupTimes < maxTime) {
        auto expectNum = catchupBinlog(startBinLog, binlogHigh , _slots);
        if (!expectNum.ok()) {
            return false;
        }
        auto sendNum = expectNum.value();
        _binlogNum += sendNum;

        catchupTimes++;
        LOG(INFO) << "catch up finish from:" << startBinLog <<"to:" <<binlogHigh
                  << "on store:" << _storeid;
        startBinLog = binlogHigh;
        binlogHigh = kvstore->getHighestBinlogId();

        maxBinlogId = getMaxBinLog(txn);
        auto diffOffset = maxBinlogId - startBinLog;

        //  reach for distance
        if (diffOffset < distance) {
            _endBinlogid = maxBinlogId;
            LOG(INFO) << "last distance:" << diffOffset << "_curBingLog" <<
                _curBinlogid << "_endBinlog:" << _endBinlogid;
            finishCatchup = true;
            break;
        }
    }
    return  finishCatchup;
}


Status ChunkMigrateSender::sendBinlog(uint16_t maxTime) {
    LOG(INFO) << "sendBinlog begin, storeid:" << _storeid
        << " dstip:" << _dstIp << " dstport:" << _dstPort;
    PStore kvstore = _dbWithLock->store;

    auto ptxn = kvstore->createTransaction(NULL);
    auto heighBinlog = kvstore->getHighestBinlogId();

    // if no data come when migrating, no need to send end log
    if (_curBinlogid < heighBinlog) {
        bool catchUp = pursueBinLog(maxTime, _curBinlogid ,
                                heighBinlog, ptxn.value().get());
        if (!catchUp) {
            // delete dirty data on dstNode, to do wayen
           return {ErrorCodes::ERR_TIMEOUT, "catch up fail"};
        }
    }
    Status s = _svr->getMigrateManager()->lockChunks(_slots);
    if (!s.ok()) {
        return {ErrorCodes::ERR_CLUSTER, "fail lock slots"};
    }

    // LOCKs need time, so need recompute max binlog
    heighBinlog = getMaxBinLog(ptxn.value().get());
    // last binlog send
    if (_curBinlogid <  heighBinlog) {
        LOG(INFO) << "last catch up on store:" << _storeid << "_curBinglogid"
                   << _curBinlogid << "heighBinglog:" << heighBinlog;
        auto sLogNum = catchupBinlog(_curBinlogid, heighBinlog , _slots);
        if (!sLogNum.ok()) {
            LOG(ERROR) << "last catchup fail on store:" << _storeid;
            s = _svr->getMigrateManager()->unlockChunks(_slots);
            if (!s.ok()) {
                LOG(ERROR) << "unlock fail on slots in sendBinlog";
            }
            return {ErrorCodes::ERR_NETWORK, "send last binglog fail"};
        }
        _binlogNum += sLogNum.value();
    }

    LOG(INFO) << "ChunkMigrateSender::sendBinlog over, remote_addr "
              << _client->getRemoteRepr() << ":" <<_client->getRemotePort()
              << " curbinlog:" << _curBinlogid << " endbinlog:" << heighBinlog
              << "send binlog total num is:" << _binlogNum;

    return { ErrorCodes::ERR_OK, ""};
}


Status ChunkMigrateSender::sendOver() {
    std::stringstream ss2;
    Command::fmtMultiBulkLen(ss2, 3);
    Command::fmtBulk(ss2, "migrateend");
    Command::fmtBulk(ss2, _slots.to_string());
    Command::fmtBulk(ss2, std::to_string(_dstStoreid));

    std::string stringtoWrite = ss2.str();
    Status s = _client->writeData(stringtoWrite);
    if (!s.ok()) {
        LOG(ERROR) << " writeData failed:" << s.toString()
                     << ",data:" << ss2.str();
        return s;
    }

    // if check meta data change successfully
    if (checkSlotsBlongDst(_slots)) {
        return { ErrorCodes::ERR_OK, ""};
    }

    uint32_t secs = _cfg->timeoutSecBinlogWaitRsp;
    Expected<std::string> exptOK = _client->readLine(std::chrono::seconds(secs));

    if (!exptOK.ok()) {
        LOG(ERROR) <<  " dst Store:" << _dstStoreid
                     << " readLine failed:" << exptOK.status().toString()
                     << "; Size:" << stringtoWrite.size()
                     << "; Seconds:" << secs;
        // maybe miss message in network
        return { ErrorCodes::ERR_CLUSTER, "missing package" };

    } else if (exptOK.value() != "+OK") { // TODO: two phase commit protocol
        LOG(ERROR) << "get response of migrateend failed "
            << "dstStoreid:" << _dstStoreid
            << " rsp:" << exptOK.value();
        return{ ErrorCodes::ERR_NETWORK, "bad return string" };
    }

    // set  meta data of source node
    s = _clusterState->setSlots(_dstNode, _slots);
    if (!s.ok()) {
        LOG(ERROR) << "set myself meta data fail on slots";
        return { ErrorCodes::ERR_CLUSTER, "set slot dstnode fail" };
    }

    return { ErrorCodes::ERR_OK, ""};
}


Expected<uint64_t> ChunkMigrateSender::deleteChunk(uint32_t  chunkid) {
    LOG(INFO) << "deleteChunk begin on chunkid:" << chunkid;
    PStore kvstore = _dbWithLock->store;
    auto ptxn = kvstore->createTransaction(NULL);
    if (!ptxn.ok()) {
        return ptxn.status();
    }
    auto cursor = std::move(ptxn.value()->createSlotsCursor(chunkid, chunkid+1));
    bool over = false;
    uint32_t deleteNum = 0;

    while (true) {
        Expected<Record> expRcd = cursor->next();
        if (expRcd.status().code() == ErrorCodes::ERR_EXHAUST) {
            over = true;
            break;
        }

        if (!expRcd.ok()) {
            LOG(ERROR) << "delete cursor error on chunkid:" << chunkid;
            return false;
        }

        Record &rcd = expRcd.value();
        const RecordKey &rcdKey = rcd.getRecordKey();

        auto s = ptxn.value()->delKV(rcdKey.encode());
        if (!s.ok()) {
            LOG(ERROR) << "delete key fail";
            continue;
        }
        deleteNum++;
    }

    auto s = ptxn.value()->commit();
    // todo: retry_times
    if (!s.ok()) {
        return s.status();
    }
    LOG(INFO) << "deleteChunk chunkid:" << chunkid
        << " num:" << deleteNum << " is_over:" << over;
    return deleteNum;
}

bool ChunkMigrateSender::deleteChunks(const std::bitset<CLUSTER_SLOTS>& slots) {
    size_t idx = 0;
    while (idx < slots.size()) {
        if (slots.test(idx)) {
            auto v = deleteChunk(idx);
            if (!v.ok()) {
                LOG(ERROR) << "delete slot:" << idx << "fail";
                return false;
            }
            _delNum += v.value();
            _delSlot ++;
        }
        idx++;
    }
    LOG(INFO) << "finish del key num: " << _delNum << " del slots num: " << _delSlot;
    if (_delNum != _snapshotKeyNum + _binlogNum) {
        LOG(ERROR) << "del num: " << _delNum
                   << "is not equal to (snaphotKey+binlog) "
                   << "snapshotKey num: " << _snapshotKeyNum
                   << "binlog num: " << _binlogNum;
    } else {
        _consistency = true;
        LOG(INFO) << "consistent OK on storeid: " << _storeid;
    }
    return true;
}


} // end namespace

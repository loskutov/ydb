#include "tx_write.h"

namespace NKikimr::NColumnShard {

bool TTxWrite::InsertOneBlob(TTransactionContext& txc, const NOlap::TWideSerializedBatch& batch, const TWriteId writeId) {
    NKikimrTxColumnShard::TLogicalMetadata meta;
    meta.SetNumRows(batch->GetRowsCount());
    meta.SetRawBytes(batch->GetRawBytes());
    meta.SetDirtyWriteTimeSeconds(batch.GetStartInstant().Seconds());
    meta.SetSpecialKeysRawData(batch->GetSpecialKeysSafe().SerializeToString());

    const auto& blobRange = batch.GetRange();
    Y_ABORT_UNLESS(blobRange.GetBlobId().IsValid());

    // First write wins
    TBlobGroupSelector dsGroupSelector(Self->Info());
    NOlap::TDbWrapper dbTable(txc.DB, &dsGroupSelector);

    const auto& writeMeta = batch.GetAggregation().GetWriteData()->GetWriteMeta();
    auto schemeVersion = batch.GetAggregation().GetWriteData()->GetData()->GetSchemaVersion();
    auto tableSchema = Self->TablesManager.GetPrimaryIndex()->GetVersionedIndex().GetSchemaVerified(schemeVersion);

    NOlap::TInsertedData insertData((ui64)writeId, writeMeta.GetTableId(), writeMeta.GetDedupId(), blobRange, meta, tableSchema->GetVersion(), batch->GetData());
    bool ok = Self->InsertTable->Insert(dbTable, std::move(insertData));
    if (ok) {
        Self->UpdateInsertTableCounters();
        return true;
    }
    return false;
}

bool TTxWrite::Execute(TTransactionContext& txc, const TActorContext&) {
    TMemoryProfileGuard mpg("TTxWrite::Execute");
    NActors::TLogContextGuard logGuard = NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD)("tablet_id", Self->TabletID())("tx_state", "execute");
    ACFL_DEBUG("event", "start_execute");
    const NOlap::TWritingBuffer& buffer = PutBlobResult->Get()->MutableWritesBuffer();
    for (auto&& aggr : buffer.GetAggregations()) {
        const auto& writeMeta = aggr->GetWriteData()->GetWriteMeta();
        Y_ABORT_UNLESS(Self->TablesManager.IsReadyForWrite(writeMeta.GetTableId()));
        txc.DB.NoMoreReadsForTx();
        TWriteOperation::TPtr operation;
        if (writeMeta.HasLongTxId()) {
            AFL_VERIFY(aggr->GetSplittedBlobs().size() == 1)("count", aggr->GetSplittedBlobs().size());
        } else {
            operation = Self->OperationsManager->GetOperation((TWriteId)writeMeta.GetWriteId());
            Y_ABORT_UNLESS(operation);
            Y_ABORT_UNLESS(operation->GetStatus() == EOperationStatus::Started);
        }

        auto writeId = TWriteId(writeMeta.GetWriteId());
        if (!operation) {
            NIceDb::TNiceDb db(txc.DB);
            writeId = Self->GetLongTxWrite(db, writeMeta.GetLongTxIdUnsafe(), writeMeta.GetWritePartId());
            aggr->AddWriteId(writeId);
        }

        for (auto&& i : aggr->GetSplittedBlobs()) {
            if (operation) {
                writeId = Self->BuildNextWriteId(txc);
                aggr->AddWriteId(writeId);
            }

            if (!InsertOneBlob(txc, i, writeId)) {
                LOG_S_DEBUG(TxPrefix() << "duplicate writeId " << (ui64)writeId << TxSuffix());
                Self->IncCounter(COUNTER_WRITE_DUPLICATE);
            }
        }
    }

    NOlap::TBlobManagerDb blobManagerDb(txc.DB);
    AFL_VERIFY(buffer.GetAddActions().size() == 1);
    for (auto&& i : buffer.GetAddActions()) {
        i->OnExecuteTxAfterWrite(*Self, blobManagerDb, true);
    }
    for (auto&& i : buffer.GetRemoveActions()) {
        i->OnExecuteTxAfterRemoving(blobManagerDb, true);
    }
    for (auto&& aggr : buffer.GetAggregations()) {
        const auto& writeMeta = aggr->GetWriteData()->GetWriteMeta();
        if (!writeMeta.HasLongTxId()) {
            auto operation = Self->OperationsManager->GetOperation((TWriteId)writeMeta.GetWriteId());
            Y_ABORT_UNLESS(operation);
            Y_ABORT_UNLESS(operation->GetStatus() == EOperationStatus::Started);
            operation->OnWriteFinish(txc, aggr->GetWriteIds());
            if (operation->GetBehaviour() == EOperationBehaviour::InTxWrite) {
                NKikimrTxColumnShard::TCommitWriteTxBody proto;
                proto.SetLockId(operation->GetLockId());
                TString txBody;
                Y_ABORT_UNLESS(proto.SerializeToString(&txBody));
                auto result = Self->GetProgressTxController().ProposeTransaction(TTxController::TBasicTxInfo(NKikimrTxColumnShard::TX_KIND_COMMIT_WRITE, operation->GetLockId()), txBody, writeMeta.GetSource(), operation->GetCookie(), txc);
                AFL_VERIFY(!result.IsError());
                Results.emplace_back(NEvents::TDataEvents::TEvWriteResult::BuildPrepared(
                    Self->TabletID(), result.GetFullTxInfoVerified().TxId, Self->GetProgressTxController().BuildCoordinatorInfo(result.GetFullTxInfoVerified())));
            } else {
                NKikimrDataEvents::TLock lock;
                lock.SetLockId(operation->GetLockId());
                lock.SetDataShard(Self->TabletID());
                lock.SetGeneration(1);
                lock.SetCounter(1);

                Results.emplace_back(NEvents::TDataEvents::TEvWriteResult::BuildCompleted(Self->TabletID(), operation->GetLockId(), lock));
            }
        } else {
            Y_ABORT_UNLESS(aggr->GetWriteIds().size() == 1);
            Results.emplace_back(std::make_unique<TEvColumnShard::TEvWriteResult>(Self->TabletID(), writeMeta, (ui64)aggr->GetWriteIds().front(), NKikimrTxColumnShard::EResultStatus::SUCCESS));
        }
    }
    return true;
}

void TTxWrite::Complete(const TActorContext& ctx) {
    TMemoryProfileGuard mpg("TTxWrite::Complete");
    NActors::TLogContextGuard logGuard = NActors::TLogContextBuilder::Build(NKikimrServices::TX_COLUMNSHARD)("tablet_id", Self->TabletID())("tx_state", "complete");
    const auto now = TMonotonic::Now();
    const NOlap::TWritingBuffer& buffer = PutBlobResult->Get()->MutableWritesBuffer();
    for (auto&& i : buffer.GetAddActions()) {
        i->OnCompleteTxAfterWrite(*Self, true);
    }
    for (auto&& i : buffer.GetRemoveActions()) {
        i->OnCompleteTxAfterRemoving(true);
    }
    AFL_VERIFY(buffer.GetAggregations().size() == Results.size());
    for (ui32 i = 0; i < buffer.GetAggregations().size(); ++i) {
        const auto& writeMeta = buffer.GetAggregations()[i]->GetWriteData()->GetWriteMeta();
        auto operation = Self->OperationsManager->GetOperation((TWriteId)writeMeta.GetWriteId());
        if (operation) {
            Self->GetProgressTxController().CompleteTransaction(operation->GetLockId(), ctx);
            ctx.Send(writeMeta.GetSource(), Results[i].release(), 0, operation->GetCookie());
        } else {
            ctx.Send(writeMeta.GetSource(), Results[i].release());
        }
        Self->CSCounters.OnWriteTxComplete(now - writeMeta.GetWriteStartInstant());
        Self->CSCounters.OnSuccessWriteResponse();
    }

}

}

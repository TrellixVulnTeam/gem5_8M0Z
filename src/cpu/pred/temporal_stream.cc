#include "cpu/pred/temporal_stream.hh"

#include <bitset>
#include <map>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/TemporalStream.hh"

/*
note:
- btbUpdate, update & squash all takes a void* bp_history,
    recast it as TSHistory object, and reads info from it.
- uncondBranch, lookup takes a void* bp_history,
allocates a new TSHistory and save
    it to bp_history.
- since we use a base predictor, we use TSHistory->baseHistory to
    function as bp_history for the base predictor.
- for most of the methods, we simply call the corresponding
    method of the base predictor.
- only for update & lookup, there's additional work
    related with the TemporalStreamBP
- FIXME: not sure whether to handle the TS circular
    buffer in uncondBranch()
*/
namespace gem5
{

    namespace branch_prediction
    {
        TemporalStreamBP::TemporalStreamBP(
            const TemporalStreamBPParams& params
        ):
            // initialted in BranchPredictor.py::TemporalStreamPredictor.
            BPredUnit(params),
            // also change here if want to use another base predictor.
            basePredictor(params.base_predictor),
            bufferSize(params.circular_buffer_size)
        {
            DPRINTF(TemporalStream, "Start initialization\n");
            replayFlag = false;

            // bufferHead = 0;

            // bufferTail = 0;

            // setup the circular buffer
            // circularBuffer.resize(bufferSize);
            bufferSize++;
            // for (int i = 0; i < bufferSize; ++i)
            //     circularBuffer[i] = HTB_INIT;

            DPRINTF(TemporalStream, "Initialized successfully\n");
            DPRINTF(TemporalStream, "basePredictor: %p\n", basePredictor);
            DPRINTF(TemporalStream, "basePredictor: %s\n",
            typeid(basePredictor).name());
        }

        std::bitset<TS_KEY_SIZE> TemporalStreamBP::ts_idx(Addr PC,
        ThreadID tid) {
            std::bitset<TS_KEY_SIZE> pc = std::bitset<TS_KEY_SIZE>(PC);
            return pc | (ts_gh[tid] << 64);
        }

        void TemporalStreamBP::btbUpdate(
            ThreadID tid,
            Addr branch_addr,
            void*& bp_history
        )
        {
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x: Enter btbUpdate \n",
                (int16_t)tid,
                (uint64_t)branch_addr);
            // TSHistory *history = static_cast<TSHistory *>(bp_history);
            TSHistory *history = static_cast<TSHistory *>(bp_history);
            basePredictor->btbUpdate(
                tid,
                branch_addr,
                history->baseHistory
            );
            // DPRINTF(TemporalStream, "Exit btbUpdate \n");

        }

        bool TemporalStreamBP::lookup(
            ThreadID tid,
            Addr branch_addr,
            void*& bp_history
        )
        {
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x: Enter lookup \n",
                (int16_t)tid,
                (uint64_t)branch_addr);
            TSHistory *history = new TSHistory;
            bool baseOutcome = basePredictor->lookup(
                tid, branch_addr, history->baseHistory
            );

            bool tsOutcome;

            // if (replayFlag && circularBuffer[bufferHead++%bufferSize]==0)
            if (replayFlag) {
                ++bufferHead;
                if (!*bufferHead)
                    tsOutcome = !baseOutcome;
                else
                    tsOutcome = baseOutcome;
            }
            else {
                tsOutcome = baseOutcome;
            }
            history->baseOutcome = baseOutcome;
            history->tsOutcome = tsOutcome;
            history->uncond = false;
            history->trigPC = branch_addr;
            bp_history = static_cast<void*>(history);
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x: lookup generated bp_history@%p \n",
                (int16_t)tid,
                (uint64_t)branch_addr,
                bp_history);
            // DPRINTF(TemporalStream, "Exit lookup \n");
            return tsOutcome;
        }

        void TemporalStreamBP::uncondBranch(
            ThreadID tid,
            Addr pc,
            void*& bp_history
        )
        {
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x: Enter uncondBranch\n",
                (int16_t)tid,
                (uint64_t)pc);
            TSHistory *history = new TSHistory;
            history->baseOutcome = true;
            history->tsOutcome = true;
            history->uncond = true;
            history->trigPC = -1;
            // void *baseHistory = (history->baseHistory);
            basePredictor->uncondBranch(tid, pc, history->baseHistory);
            bp_history = static_cast<void*>(history);
            DPRINTF(TemporalStream, "TShistory %p\n", history);
            DPRINTF(TemporalStream, "baseHistory %p\n", history->baseHistory);
            DPRINTF(TemporalStream, "Exit uncondBranch \n");
        }

        void TemporalStreamBP::update(
            ThreadID tid,
            Addr branch_addr,
            bool taken,
            void* bp_history,
            bool squashed,
            const StaticInstPtr& inst,
            Addr corrTarget
        )
        {
            TSHistory *history = static_cast<TSHistory *>(bp_history);
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x, trigPC=%x, \
                squash=%d: Enter update \n\t bp_history@%p \n",
                (int16_t)tid,
                (uint64_t)branch_addr,
                (uint64_t)history->trigPC,
                squashed,
                bp_history);

            assert(history->baseHistory);

            //update_base_predictor();
            basePredictor->update(
                tid, branch_addr, taken,
                history->baseHistory, squashed,
                inst, corrTarget
            );

            if (squashed) {
                // should do sth?
                return;
            }

            // update_features();
            if (ts_gh.find(tid) == ts_gh.end()) ts_gh[tid] = 0;
            ts_gh[tid] <<= 1;
            if (taken){
                ts_gh[tid][0] = 1;
            }
            // DPRINTF(TemporalStream, "basePredictor update complete\n");

            // update ts
            // circularBuffer[
            //     ++bufferTail%bufferSize
            // ] = (history->baseOutcome==taken);
            circularBuffer.push_back(history->baseOutcome==taken);
            bufferTail = circularBuffer.end();
            --bufferTail;

            // DPRINTF(TemporalStream, "circularBuffer update complete\n");

            if (replayFlag && history->tsOutcome != taken)
                replayFlag = false;
            if (history->baseOutcome != taken) {
                // FIXME: concatenated tid with ts_gh
                // key = key_from_features();
                std::bitset<TS_KEY_SIZE> idx = ts_idx(branch_addr, tid);

                if (!replayFlag) {
                    auto iter = headTable[tid].find(idx);
                    if (
                    (iter!=headTable[tid].end())
                    // && (iter->second!=HTB_INIT)
                    ){
                        bufferHead = iter->second;
                        replayFlag = true;
                    }
                }

                // FIXME: in predictor.cc writes
                // -------------------------
                // tstable[idx] = ts.end();
                // --tstable[idx];
                // -------------------------
                // this is because they assume
                // the CB is infinite,
                // while we have a param SIZE here.
                // so the tail is maintained by
                // "++bufferTail%bufferSize" here,
                // which adds tail by 1 and return it.
                headTable[tid][idx] = bufferTail;
            }
            // history->baseHistory deleted during basePredictor->update
            // if (history->uncond && squashed){
            //     DPRINTF(TemporalStream,
            //     "tid=%x, PC=%x, trigPC=%x: Skipping uncondBranch\
            //     first delete\n",
            //     (int16_t)tid,
            //     (uint64_t)branch_addr,
            //     (uint64_t)history->trigPC);
            // }
            // else {
            //     delete history;
            // }
            delete history;
            DPRINTF(TemporalStream,
                "tid=%x, PC=%x, trigPC=%x: Exit update\n",
                (int16_t)tid,
                (uint64_t)branch_addr,
                (uint64_t)history->trigPC);

        }

        void TemporalStreamBP::squash(
            ThreadID tid,
            void* bp_history
        )
        {

            TSHistory *history = static_cast<TSHistory *>(bp_history);

            DPRINTF(TemporalStream,
                "tid=%x, PC=%x: Enter squash \n\t bp_hist@%p \n",
                (int16_t)tid,
                (uint64_t)history->trigPC,
                bp_history);

            basePredictor->squash(
                tid,
                history->baseHistory
            );
            delete history;
            DPRINTF(TemporalStream, "Exit squash \n");
        }

    } // namespace branch_prediction
} // namespace gem5

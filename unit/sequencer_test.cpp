#include <iostream>

#include <boost/atomic.hpp>
#include <boost/thread.hpp>

#include <disruptor/sequencer.h>

#include <gtest/gtest.h>

#define BUFFER_SIZE 4

namespace disruptor {
namespace test {

class SequencerFixture : public ::testing::Test
{
protected:
    SequencerFixture()
        : sequencer(BUFFER_SIZE,
                  kSingleThreadedStrategy,
                  kSleepingStrategy)
        , gating_sequence(INITIAL_CURSOR_VALUE)
    {
        std::vector<Sequence*> sequences;
        sequences.push_back(&gating_sequence);
        sequencer.setGatingSequences(sequences);
    }

    ~SequencerFixture() {}

    void fillBuffer()
    {
        for (int i = 0; i < BUFFER_SIZE; i++) {
            int64_t sequence = sequencer.next();
            sequencer.publish(sequence);
        }
    }

    Sequencer sequencer;
    Sequence gating_sequence;
};


TEST_F(SequencerFixture, testStartWithValueInitialized)
{
    EXPECT_EQ(INITIAL_CURSOR_VALUE, sequencer.getCursor());
}

TEST_F(SequencerFixture, testGetPublishFirstSequence)
{
    const int64_t sequence = sequencer.next();
    EXPECT_EQ(INITIAL_CURSOR_VALUE, sequencer.getCursor());
    EXPECT_EQ(0, sequence);

    sequencer.publish(sequence);
    EXPECT_EQ(sequence, sequencer.getCursor());
}

TEST_F(SequencerFixture, testIndicateAvailableCapacity)
{
    EXPECT_TRUE(sequencer.hasAvailableCapacity());
}

TEST_F(SequencerFixture, testIndicateNoAvailableCapacity)
{
    fillBuffer();
    EXPECT_FALSE(sequencer.hasAvailableCapacity());
}

TEST_F(SequencerFixture, testForceClaimSequence)
{
    const int64_t claim_sequence = 3;
    const int64_t sequence = sequencer.claim(claim_sequence);

    EXPECT_EQ(INITIAL_CURSOR_VALUE, sequencer.getCursor());
    EXPECT_EQ(claim_sequence, sequence);

    sequencer.forcePublish(sequence);
    EXPECT_EQ(claim_sequence, sequencer.getCursor());
}

TEST_F(SequencerFixture, testCapacityChange)
{
    EXPECT_EQ(BUFFER_SIZE, sequencer.capacity());

    std::vector<Sequence*> dependents(0);
    SequenceBarrierPtr barrier = sequencer.newBarrier(dependents);

    sequencer.publish(sequencer.next());
    sequencer.publish(sequencer.next());
    const int64_t sequence = sequencer.next();
    sequencer.publish(sequence);

    EXPECT_EQ(1, sequencer.remainingCapacity());
    EXPECT_EQ(3, sequencer.occupiedCapacity());

    EXPECT_EQ(sequence, barrier->waitFor(INITIAL_CURSOR_VALUE + 1LL));
}

TEST_F(SequencerFixture, testPublishSequenceBatch)
{
    const int batch_size = 3;
    BatchDescriptor batch_descriptor(batch_size);
    sequencer.next(&batch_descriptor);

    EXPECT_EQ(INITIAL_CURSOR_VALUE, sequencer.getCursor());
    EXPECT_EQ(INITIAL_CURSOR_VALUE + batch_size, batch_descriptor.end());
    EXPECT_EQ(batch_size, batch_descriptor.size());

    sequencer.publish(batch_descriptor);
    EXPECT_EQ(INITIAL_CURSOR_VALUE + batch_size, sequencer.getCursor());
}

TEST_F(SequencerFixture, testWaitOnSequence)
{
    std::vector<Sequence*> dependents(0);
    SequenceBarrierPtr barrier = sequencer.newBarrier(dependents);

    const int64_t sequence = sequencer.next();
    sequencer.publish(sequence);

    EXPECT_EQ(barrier->waitFor(sequence), sequence);
}

TEST_F(SequencerFixture, testWaitOnSequenceShowingBatchingEffect)
{
    std::vector<Sequence*> dependents(0);
    SequenceBarrierPtr barrier = sequencer.newBarrier(dependents);

    sequencer.publish(sequencer.next());
    sequencer.publish(sequencer.next());

    const int64_t sequence = sequencer.next();
    sequencer.publish(sequence);

    EXPECT_EQ(barrier->waitFor(INITIAL_CURSOR_VALUE + 1LL), sequence);
}

class SignalWaitingProcessorPublisher
{
    private:
        Sequence* gating_sequence_;
        ISequenceBarrier* barrier_;
        boost::atomic<bool>* waiting_;
        boost::atomic<bool>* completed_;

    public:
        SignalWaitingProcessorPublisher(
            Sequence* gating_sequence,
            ISequenceBarrier* barrier,
            boost::atomic<bool>* waiting,
            boost::atomic<bool>* completed)
            : gating_sequence_(gating_sequence)
            , barrier_(barrier)
            , waiting_(waiting)
            , completed_(completed)
        {
        }

        void operator() ()
        {
            waiting_->store(false);
            EXPECT_EQ(INITIAL_CURSOR_VALUE + 1LL, barrier_->waitFor(INITIAL_CURSOR_VALUE + 1LL));
            gating_sequence_->set(INITIAL_CURSOR_VALUE + 1LL);
            completed_->store(true);
        }

};

TEST_F(SequencerFixture, testSignalWaitingProcessorWhenSequenceIsPublished)
{
    std::vector<Sequence*> dependents(0);
    SequenceBarrierPtr barrier = sequencer.newBarrier(dependents);

    boost::atomic<bool> waiting(true);
    boost::atomic<bool> completed(false);


    SignalWaitingProcessorPublisher publisher(&gating_sequence, barrier.get(), &waiting, &completed);
    boost::thread thread(boost::ref(publisher));

    while (waiting.load()) {}
    EXPECT_EQ(INITIAL_CURSOR_VALUE, gating_sequence.get());

    sequencer.publish(sequencer.next());

    while (!completed.load()) {}
    EXPECT_EQ(INITIAL_CURSOR_VALUE + 1LL, gating_sequence.get());

    thread.join();
}

class HoldUpPublisher
{
    private:
        Sequencer* sequencer_;
        boost::atomic<bool>* waiting_;
        boost::atomic<bool>* completed_;

    public:
        HoldUpPublisher(
            Sequencer* sequencer,
            boost::atomic<bool>* waiting,
            boost::atomic<bool>* completed)
            : sequencer_(sequencer)
            , waiting_(waiting)
            , completed_(completed)
        {
        }

        void operator() ()
        {
            waiting_->store(false);
            sequencer_->publish(sequencer_->next());
            completed_->store(true);
        }

};

TEST_F(SequencerFixture, testHoldUpPublisherWhenRingIsFull)
{
    boost::atomic<bool> waiting(true);
    boost::atomic<bool> completed(false);

    fillBuffer();

    const int64_t expected_full_cursor = INITIAL_CURSOR_VALUE + BUFFER_SIZE;
    EXPECT_EQ(expected_full_cursor, sequencer.getCursor());

    HoldUpPublisher publisher(&sequencer, &waiting, &completed);
    boost::thread thread(boost::ref(publisher));


    while (waiting.load()) {}
    EXPECT_EQ(expected_full_cursor, sequencer.getCursor());

    gating_sequence.set(INITIAL_CURSOR_VALUE + 1LL);

    while (!completed.load()) {}
    EXPECT_EQ(expected_full_cursor + 1LL, sequencer.getCursor());

    thread.join();

}


}; // namepspace test
}; // namepspace disruptor

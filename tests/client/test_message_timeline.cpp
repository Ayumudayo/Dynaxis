#include <gtest/gtest.h>
#include "client/message_timeline.hpp"

TEST(MessageTimelineTest, SnapshotLiveOverlapAndLateArrivalMergeById) {
    MessageTimeline timeline;
    timeline.add_message("room-id", 30, "third");
    timeline.snapshot("room", "room-id", 30);
    timeline.add_message("room-id", 20, "second");
    timeline.add_message("room-id", 30, "third duplicate");
    timeline.add_message("room-id", 10, "first delayed");
    ASSERT_EQ(timeline.lines.size(), 3);
    EXPECT_EQ(timeline.lines[0].message_id, 10);
    EXPECT_EQ(timeline.lines[1].message_id, 20);
    EXPECT_EQ(timeline.lines[2].text, "third");
}

TEST(MessageTimelineTest, NewRoomGenerationDoesNotKeepOldHistory) {
    MessageTimeline timeline;
    timeline.snapshot("room", "old", 5);
    timeline.add_message("old", 5, "old message");
    timeline.snapshot("room", "new", 0);
    timeline.add_message("new", 6, "new message");
    ASSERT_EQ(timeline.lines.size(), 1);
    EXPECT_EQ(timeline.lines[0].room_id, "new");
}

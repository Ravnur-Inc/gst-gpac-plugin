#include "helper/common.hpp"

TEST_F(GstTestFixture, ElementPropertiesCanBeSetBackToTheirDefaults)
{
  GstElement* sink = gst_element_factory_make("gpacsink", NULL);
  g_object_set(sink, "sync", TRUE, "print-stats", TRUE, NULL);
  g_object_set(sink, "sync", FALSE, "print-stats", FALSE, NULL);

  gboolean sync = TRUE, print_stats = TRUE;
  g_object_get(sink, "sync", &sync, "print-stats", &print_stats, NULL);
  EXPECT_FALSE(sync);
  EXPECT_FALSE(print_stats);

  gst_object_unref(sink);
}

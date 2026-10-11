#include "helper/common.hpp"

#include <gio/gio.h>

#include <atomic>
#include <mutex>

// A live video track muxed with a sparse text track, the way live captions
// reach gpacsink: the text pad can stay without data for seconds while video
// keeps flowing.
class SparseTextPipeline
{
public:
  GstElement* sink = nullptr;
  GstElement* text_src = nullptr;

  SparseTextPipeline(GstElement* pipeline,
                     guint video_seconds,
                     const char* graph = "dasher:segdur=1.0:dmode=dynamic")
  {
    GstElement* video_src = gst_element_factory_make_full(
      "videotestsrc", "is-live", TRUE, "num-buffers", video_seconds * 30, NULL);
    GstElement* capsfilter = gst_element_factory_make_full(
      "capsfilter",
      "caps",
      gst_caps_from_string("video/x-raw,width=320,height=240,framerate=30/1"),
      NULL);
    GstElement* encoder = gst_element_factory_make_full("x264enc",
                                                        "tune",
                                                        4, // zerolatency
                                                        "key-int-max",
                                                        30,
                                                        "bframes",
                                                        0,
                                                        NULL);
    text_src = gst_element_factory_make_full(
      "appsrc",
      "is-live",
      TRUE,
      "format",
      GST_FORMAT_TIME,
      "caps",
      gst_caps_from_string("text/x-raw,format=utf8"),
      NULL);
    // Buffering on the text branch, as a live caption source has: without it
    // the latency the video source reports cannot be met and the aggregator
    // does not run live
    GstElement* text_queue = gst_element_factory_make("queue", NULL);
    sink = gst_element_factory_make_full("gpacsink",
                                         "graph",
                                         graph,
                                         "destination",
                                         "live.m3u8",
                                         NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     video_src,
                     capsfilter,
                     encoder,
                     text_src,
                     text_queue,
                     sink,
                     NULL);
    if (!gst_element_link_many(video_src, capsfilter, encoder, sink, NULL))
      g_error("Failed to link elements");
    GstPad* text_pad = gst_element_request_pad_simple(sink, "subtitle_%u");
    GstPad* src_pad = gst_element_get_static_pad(text_queue, "src");
    if (!gst_element_link(text_src, text_queue) || !text_pad ||
        gst_pad_link(src_pad, text_pad) != GST_PAD_LINK_OK)
      g_error("Failed to link the text source");
    gst_object_unref(src_pad);
    gst_object_unref(text_pad);

    g_signal_connect(
      sink, "get-segment", G_CALLBACK(&SparseTextPipeline::on_segment), this);

    // Video buffers the sink has accepted: a sink pad that is full blocks the
    // encoder's push, so this stops growing while the element holds video back
    GstPad* encoder_src = gst_element_get_static_pad(encoder, "src");
    gst_pad_add_probe(
      encoder_src,
      GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo*, gpointer data) {
        static_cast<SparseTextPipeline*>(data)->video_buffers++;
        return GST_PAD_PROBE_OK;
      },
      this,
      NULL);
    gst_object_unref(encoder_src);
  }

  void PushText(const char* text, GstClockTime pts, GstClockTime duration)
  {
    GstBuffer* buffer = gst_buffer_new_memdup(text, strlen(text));
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = duration;
    GstFlowReturn ret;
    g_signal_emit_by_name(text_src, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);
  }

  void PushGap(GstClockTime pts, GstClockTime duration)
  {
    gst_element_send_event(text_src, gst_event_new_gap(pts, duration));
  }

  void EndText()
  {
    GstFlowReturn ret;
    g_signal_emit_by_name(text_src, "end-of-stream", &ret);
  }

  // Video buffers accepted by the sink so far
  int VideoBuffers() { return video_buffers.load(); }

  // Concatenated content of every WebVTT segment, once the pipeline is done
  std::string Text()
  {
    std::lock_guard<std::mutex> lock(mutex);
    std::string text;
    for (GOutputStream* stream : text_streams) {
      g_output_stream_close(stream, NULL, NULL);
      auto* mem = G_MEMORY_OUTPUT_STREAM(stream);
      text.append(
        static_cast<const char*>(g_memory_output_stream_get_data(mem)),
        g_memory_output_stream_get_data_size(mem));
    }
    return text;
  }

  int TextSegments()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return (int)text_streams.size();
  }

  ~SparseTextPipeline()
  {
    for (GOutputStream* stream : text_streams)
      g_object_unref(stream);
  }

private:
  std::atomic<int> video_buffers{ 0 };
  std::mutex mutex;
  std::vector<GOutputStream*> text_streams;

  static GOutputStream* on_segment(GstElement* self,
                                   const gchar* label,
                                   gpointer user_data)
  {
    auto* pipeline = static_cast<SparseTextPipeline*>(user_data);
    GOutputStream* stream = g_memory_output_stream_new_resizable();
    if (g_str_has_suffix(label, ".vtt")) {
      std::lock_guard<std::mutex> lock(pipeline->mutex);
      pipeline->text_streams.push_back(G_OUTPUT_STREAM(g_object_ref(stream)));
    }
    return stream;
  }
};

TEST_F(GstTestFixture, AggregationDeadlineWithSilentTextPad)
{
  SparseTextPipeline sparse(pipeline, 6);
  g_object_set(sparse.sink, "aggregation-deadline", 100, NULL);
  this->StartPipeline();

  // One caption, then nothing on the text pad while the video plays
  sparse.PushText("hello", 0, 500 * GST_MSECOND);
  g_usleep(4 * G_USEC_PER_SEC);

  // Without a deadline the element waits for the text pad and holds the video
  // back after a few buffers; with it, the video keeps flowing in.
  EXPECT_GE(sparse.VideoBuffers(), 2 * 30);

  sparse.EndText();
  this->WaitForEOS();
}

// Every "-->" timing line of a WebVTT document, with the line that follows it
static std::vector<std::pair<std::string, std::string>>
ParseCues(const std::string& vtt)
{
  std::vector<std::pair<std::string, std::string>> cues;
  gchar** lines = g_strsplit(vtt.c_str(), "\n", -1);
  for (int i = 0; lines[i]; i++) {
    if (strstr(lines[i], "-->"))
      cues.emplace_back(lines[i], lines[i + 1] ? lines[i + 1] : "");
  }
  g_strfreev(lines);
  return cues;
}

TEST_F(GstTestFixture, WebVttSamplesCarryCuesAndGaps)
{
  SparseTextPipeline sparse(
    pipeline, 5, "dasher:segdur=1.0:dmode=dynamic:rawsub=true");
  g_object_set(sparse.sink,
               "aggregation-deadline",
               100,
               "webvtt-samples",
               TRUE,
               NULL);
  this->StartPipeline();

  // Two cues, the time between and after them covered by gaps
  sparse.PushGap(0, 500 * GST_MSECOND);
  sparse.PushText("hello", 500 * GST_MSECOND, GST_SECOND);
  for (int i = 0; i < 17; i++)
    sparse.PushGap(1500 * GST_MSECOND + i * 100 * GST_MSECOND,
                   100 * GST_MSECOND);
  sparse.PushText("world", 3200 * GST_MSECOND, GST_SECOND);
  sparse.PushGap(4200 * GST_MSECOND, 800 * GST_MSECOND);

  // End the text track with the video, as a live caption track does
  g_usleep(5 * G_USEC_PER_SEC);
  sparse.EndText();
  this->WaitForEOS();

  // Both cues, and nothing from the gaps: no cue without text
  std::string vtt = sparse.Text();
  EXPECT_NE(vtt.find("hello"), std::string::npos) << vtt;
  EXPECT_NE(vtt.find("world"), std::string::npos) << vtt;
  for (auto& [timing, text] : ParseCues(vtt))
    EXPECT_FALSE(text.empty()) << "empty cue at " << timing << "\n" << vtt;

  // The text track is segmented all the way, not only up to the last cue
  EXPECT_GE(sparse.TextSegments(), 5);
}

TEST_F(GstTestFixture, VttCueSettingsGivenToEveryCue)
{
  SparseTextPipeline sparse(
    pipeline, 3, "dasher:segdur=1.0:dmode=dynamic:rawsub=true");
  g_object_set(sparse.sink,
               "aggregation-deadline",
               100,
               "webvtt-samples",
               TRUE,
               "vtt-cue-settings",
               "align:left position:10% line:-2",
               NULL);
  this->StartPipeline();

  sparse.PushText("one", 200 * GST_MSECOND, 600 * GST_MSECOND);
  sparse.PushGap(800 * GST_MSECOND, 700 * GST_MSECOND);
  sparse.PushText("two", 1500 * GST_MSECOND, GST_SECOND);
  sparse.PushGap(2500 * GST_MSECOND, 500 * GST_MSECOND);
  g_usleep(3 * G_USEC_PER_SEC);
  sparse.EndText();
  this->WaitForEOS();

  std::string vtt = sparse.Text();
  auto cues = ParseCues(vtt);
  EXPECT_GE(cues.size(), 2u) << vtt;
  for (auto& [timing, text] : cues)
    EXPECT_NE(timing.find(" align:left position:10% line:-2"),
              std::string::npos)
      << timing;
}

TEST_F(GstTestFixture, DrainGapsHandsAQueuedBurstToGpacAtOnce)
{
  SparseTextPipeline sparse(
    pipeline, 5, "dasher:segdur=1.0:dmode=dynamic:rawsub=true");
  g_object_set(sparse.sink,
               "aggregation-deadline",
               100,
               "webvtt-samples",
               TRUE,
               "drain-gaps",
               TRUE,
               NULL);
  this->StartPipeline();

  // A cue, then 4 s of text track progress as a burst of short GAPs, queued
  // at once. Without drain-gaps one GAP is converted per aggregation (about
  // 40 a second here), so after 2.5 s the text track has advanced about 1 s
  // and its segments lag the video's; with it the whole burst reaches GPAC
  // in one aggregation.
  sparse.PushText("burst", 0, 500 * GST_MSECOND);
  for (int i = 0; i < 350; i++)
    sparse.PushGap(500 * GST_MSECOND + i * 10 * GST_MSECOND,
                   10 * GST_MSECOND);
  g_usleep(2500 * 1000);
  EXPECT_GE(sparse.TextSegments(), 2);

  g_usleep(2500 * 1000);
  sparse.EndText();
  this->WaitForEOS();

  // The burst carries no cue of its own
  std::string vtt = sparse.Text();
  for (auto& [timing, text] : ParseCues(vtt))
    EXPECT_EQ(text, "burst") << timing << "\n" << vtt;
}

TEST_F(GstTestFixture, WebVttCueTextAndSettingsCannotBreakTheCue)
{
  SparseTextPipeline sparse(
    pipeline, 2, "dasher:segdur=1.0:dmode=dynamic:rawsub=true");
  g_object_set(sparse.sink,
               "aggregation-deadline",
               100,
               "webvtt-samples",
               TRUE,
               NULL);

  // A line break would end the timing line: the settings are refused
  g_object_set(sparse.sink, "vtt-cue-settings", "line:-2\nalign:left", NULL);
  gchar* settings = NULL;
  g_object_get(sparse.sink, "vtt-cue-settings", &settings, NULL);
  EXPECT_EQ(settings, nullptr);
  g_free(settings);

  this->StartPipeline();
  // An empty line would end the cue, and "-->" read as a timing line
  sparse.PushText("first\r\n\nsecond --> third\n", 200 * GST_MSECOND,
                  600 * GST_MSECOND);
  sparse.PushGap(800 * GST_MSECOND, 1200 * GST_MSECOND);
  g_usleep(2 * G_USEC_PER_SEC);
  sparse.EndText();
  this->WaitForEOS();

  std::string vtt = sparse.Text();
  EXPECT_NE(vtt.find("\nfirst\nsecond --&gt; third\n\n"), std::string::npos)
    << vtt;
  EXPECT_EQ(ParseCues(vtt).size(), 1u) << vtt;
}

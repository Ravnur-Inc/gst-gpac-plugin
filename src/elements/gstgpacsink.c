/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Deniz Ugur, Romain Bouqueau, Sohaib Larbi
 *			Copyright (c) Motion Spell
 *				All rights reserved
 *
 *  This file is part of the GPAC/GStreamer wrapper
 *
 *  This GPAC/GStreamer wrapper is free software; you can redistribute it
 *  and/or modify it under the terms of the GNU Affero General Public License
 *  as published by the Free Software Foundation; either version 3, or (at
 *  your option) any later version.
 *
 *  This GPAC/GStreamer wrapper is distributed in the hope that it will be
 *  useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public
 *  License along with this library; see the file LICENSE.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "elements/gstgpacsink.h"

GST_DEBUG_CATEGORY_STATIC(gst_gpac_sink_debug);
#define GST_CAT_DEFAULT gst_gpac_sink_debug

// Define the element properties
#define gst_gpac_sink_parent_class parent_class
G_DEFINE_TYPE(GstGpacSink, gst_gpac_sink, GST_TYPE_BIN);

// #MARK: Properties
static void
gst_gpac_sink_set_property(GObject* object,
                           guint prop_id,
                           const GValue* value,
                           GParamSpec* pspec)
{
  GstGpacSink* gpac_sink = GST_GPAC_SINK(object);
  g_return_if_fail(GST_IS_GPAC_SINK(object));

  if (IS_TOP_LEVEL_PROPERTY(prop_id)) {
    switch (prop_id) {
      case GPAC_PROP_SYNC:
        g_object_set_property(G_OBJECT(gpac_sink->sink), pspec->name, value);
        return;
      default:
        break;
    }
  }

  // Relay the property to the internal transform element
  g_object_set_property(G_OBJECT(gpac_sink->tf), pspec->name, value);
}

static void
gst_gpac_sink_get_property(GObject* object,
                           guint prop_id,
                           GValue* value,
                           GParamSpec* pspec)
{
  GstGpacSink* gpac_sink = GST_GPAC_SINK(object);
  g_return_if_fail(GST_IS_GPAC_SINK(object));

  if (IS_TOP_LEVEL_PROPERTY(prop_id)) {
    switch (prop_id) {
      case GPAC_PROP_SYNC:
        g_object_get_property(G_OBJECT(gpac_sink->sink), pspec->name, value);
        return;
      default:
        break;
    }
  }

  // Relay the property to the internal transform element
  g_object_get_property(G_OBJECT(gpac_sink->tf), pspec->name, value);
}

// #MARK: Lifecycle
static void
gst_gpac_sink_reset(GstGpacSink* sink)
{
  // Remove the internal elements
  if (sink->tf) {
    gst_bin_remove(GST_BIN(sink), sink->tf);
    sink->tf = NULL;
  }

  if (sink->sink) {
    gst_bin_remove(GST_BIN(sink), sink->sink);
    sink->sink = NULL;
  }
}

// #MARK: Pad Management
static GstPad*
gst_gpac_sink_request_new_pad(GstElement* element,
                              GstPadTemplate* templ,
                              const gchar* pad_name,
                              const GstCaps* caps)
{
  GstGpacSink* gpac_sink = GST_GPAC_SINK(element);
  GstPad* pad = NULL;
  GstPad* peer = NULL;

  // Request a new pad from the aggregator
  peer = gst_element_request_pad(gpac_sink->tf, templ, pad_name, caps);
  if (!peer) {
    GST_ELEMENT_ERROR(gpac_sink,
                      STREAM,
                      FAILED,
                      (NULL),
                      ("Unable to request pad name from the aggregator"));
    return NULL;
  }

  // Create a ghost pad from the aggregator
  pad = gst_ghost_pad_new_from_template(gst_pad_get_name(peer), peer, templ);
  gst_pad_set_active(pad, TRUE);
  gst_element_add_pad(element, pad);
  gst_object_unref(peer);

  return pad;
}

static void
gst_gpac_sink_release_pad(GstElement* element, GstPad* pad)
{
  GstGpacSink* gpac_sink = GST_GPAC_SINK(element);
  GstPad* peer = gst_pad_get_peer(pad);

  if (peer) {
    gst_element_release_request_pad(gpac_sink->tf, peer);
    gst_object_unref(peer);
  }

  gst_object_ref(pad);
  gst_element_remove_pad(element, pad);
  gst_pad_set_active(pad, FALSE);
  gst_object_unref(pad);
}

// #MARK: Initialization
static void
gst_gpac_sink_init(GstGpacSink* sink)
{
}

static void
gst_gpac_sink_instance_init(GstGpacSink* sink)
{
  GstElement* element = GST_ELEMENT(sink);
  GObjectClass* klass = G_OBJECT_CLASS(G_TYPE_INSTANCE_GET_CLASS(
    G_OBJECT(element), GST_TYPE_GPAC_SINK, GstGpacSinkClass));
  GstGpacParams* params = GST_GPAC_GET_PARAMS(klass);

  // Reset the sink
  gst_gpac_sink_reset(sink);

  // Treat the bin as a sink
  GST_OBJECT_FLAG_SET(sink, GST_ELEMENT_FLAG_SINK);

  // Create the transform element for the subelement
  sink->tf = g_object_new(params->private_type, NULL);

  // Create a fake sink element
  sink->sink = gst_element_factory_make("fakesink", NULL);

  // Add and link the elements
  gst_bin_add_many(GST_BIN(sink), sink->tf, sink->sink, NULL);
  gst_element_link(sink->tf, sink->sink);
}

static void
gst_gpac_sink_class_init(GstGpacSinkClass* klass)
{
  // Initialize the class
  GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass* gstelement_class = GST_ELEMENT_CLASS(klass);
  GstBinClass* gstbin_class = GST_BIN_CLASS(klass);
  parent_class = g_type_class_peek_parent(klass);

  // Install the pad templates
  gpac_install_sink_pad_templates(gstelement_class);

  // Set the pad management functions
  gstelement_class->request_new_pad =
    GST_DEBUG_FUNCPTR(gst_gpac_sink_request_new_pad);
  gstelement_class->release_pad = GST_DEBUG_FUNCPTR(gst_gpac_sink_release_pad);
}

// #MARK: Registration
static void
gst_gpac_sink_subclass_init(GstGpacSinkClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass* gstelement_class = GST_ELEMENT_CLASS(klass);
  GstGpacParams* params = GST_GPAC_GET_PARAMS(klass);

  // Set the property handlers
  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_gpac_sink_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_gpac_sink_get_property);

  // Set the property handlers
  gpac_install_global_properties(gobject_class);
  gpac_install_local_properties(
    gobject_class,
    GPAC_PROP_PRINT_STATS,
    GPAC_PROP_SYNC,
    GPAC_PROP_AGGREGATION_DEADLINE,
    GPAC_PROP_WEBVTT_SAMPLES,
    GPAC_PROP_VTT_CUE_SETTINGS,
    GPAC_PROP_DRAIN_GAPS,
    GPAC_PROP_0);

  // Add the subclass-specific properties and pad templates
  if (params->is_single) {
    // Set property blacklist
    GList* blacklist = NULL;
    if (params->info->default_options) {
      filter_option* opt = params->info->default_options;
      for (u32 i = 0; opt[i].name; i++) {
        if (opt[i].forced)
          blacklist = g_list_append(blacklist, (gpointer)opt[i].name);
      }
    }

    // Install the filter properties
    gpac_install_filter_properties(
      gobject_class, blacklist, params->info->filter_name);
    g_list_free(blacklist);

    // Install the signals
    if (params->info->signal_presets) {
      gpac_install_signals_by_presets(gobject_class,
                                      params->info->signal_presets);
    }

    // Check if we have any filter options to expose
    for (u32 i = 0; i < G_N_ELEMENTS(filter_options); i++) {
      filter_option_overrides* opts = &filter_options[i];
      if (g_strcmp0(opts->filter_name, params->info->filter_name) == 0) {
        for (u32 j = 0; opts->options[j]; j++) {
          guint32 prop_id = opts->options[j];
          gpac_install_local_properties(gobject_class, prop_id, GPAC_PROP_0);
        }
        break;
      }
    }

    // Register the internal element type
    params->private_type =
      gst_gpac_tf_register_custom(params->info, TRUE, TRUE);
  } else {
    gpac_install_src_pad_templates(gstelement_class);
    gpac_install_local_properties(
      gobject_class, GPAC_PROP_GRAPH, GPAC_PROP_DESTINATION, GPAC_PROP_0);
    gpac_install_all_signals(gobject_class);

    // We need an internal version of the transform element
    params->info = g_new0(subelement_info, 1);
    const subelement_info internal_element =
      GPAC_TF_SUBELEMENT("internal", NULL, NULL);
    memcpy(params->info, &internal_element, sizeof(subelement_info));

    // Register the internal element type
    params->private_type =
      gst_gpac_tf_register_custom(params->info, TRUE, FALSE);
  }

  // Set the metadata
  const gchar* longname = "gpac sink";
  if (params->is_single) {
    if (!g_strcmp0(params->info->filter_name, params->info->alias_name))
      longname = g_strdup_printf("gpac %s sink", params->info->filter_name);
    else
      longname = g_strdup_printf("gpac %s (%s) sink",
                                 params->info->alias_name,
                                 params->info->filter_name);
  }
  gst_element_class_set_static_metadata(
    gstelement_class,
    longname,
    "Aggregator/Sink",
    "Uses gpac filter session aggregate and process the incoming data",
    "Deniz Ugur <deniz.ugur@motionspell.com>");
}

gboolean
gst_gpac_sink_register(GstPlugin* plugin)
{
  GType type;
  GstGpacParams* params;
  GTypeInfo subclass_typeinfo = {
    sizeof(GstGpacSinkClass),
    NULL, // base_init
    NULL, // base_finalize
    (GClassInitFunc)gst_gpac_sink_subclass_init,
    NULL, // class_finalize
    NULL, // class_data
    sizeof(GstGpacSink),
    0,
    (GInstanceInitFunc)gst_gpac_sink_instance_init,
  };

  GST_DEBUG_CATEGORY_INIT(gst_gpac_sink_debug, "gpacsink", 0, "GPAC Sink");

  // Register the regular sink element
  GST_LOG("Registering regular gpac sink element");
  params = g_new0(GstGpacParams, 1);
  params->is_single = FALSE;
  type = g_type_register_static(
    GST_TYPE_GPAC_SINK, "GstGpacSinkRegular", &subclass_typeinfo, 0);
  g_type_set_qdata(type, GST_GPAC_PARAMS_QDATA, params);
  if (!gst_element_register(plugin, "gpacsink", GST_RANK_PRIMARY, type)) {
    GST_ERROR_OBJECT(plugin, "Failed to register regular gpac sink element");
    return FALSE;
  }

  // Register subelements
  for (u32 i = 0; i < G_N_ELEMENTS(subelements); i++) {
    subelement_info* info = &subelements[i];

    if (!(info->flags & GPAC_SE_SINK_ONLY)) {
      GST_DEBUG_OBJECT(plugin,
                       "Subelement %s is not a sink only element, skipping",
                       info->alias_name);
      continue;
    }

    // Register the regular sink element
    GST_LOG("Registering %s sink subelement", info->filter_name);
    params = g_new0(GstGpacParams, 1);
    params->is_single = TRUE;
    params->info = info;
    const gchar* name = g_strdup_printf("gpac%ssink", info->alias_name);
    const gchar* type_name =
      g_strdup_printf("GstGpacSink%c%s",
                      g_ascii_toupper(info->alias_name[0]),
                      info->alias_name + 1);

    type = g_type_register_static(
      GST_TYPE_GPAC_SINK, type_name, &subclass_typeinfo, 0);
    g_type_set_qdata(type, GST_GPAC_PARAMS_QDATA, params);
    if (!gst_element_register(plugin, name, GST_RANK_SECONDARY, type)) {
      GST_ERROR_OBJECT(
        plugin, "Failed to register %s sink subelement", info->alias_name);
      return FALSE;
    }
  }

  return TRUE;
}

GST_ELEMENT_REGISTER_DEFINE_CUSTOM(gpac_sink, gst_gpac_sink_register);

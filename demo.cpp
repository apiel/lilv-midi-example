// Copyright 2007-2019 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "lilv/lilv.h"

#include "lv2/core/lv2.h"

// #include <lv2/core/lv2_util.h>
// #include <lv2/lv2plug.in/ns/ext/atom/atom.h>
// #include <lv2/lv2plug.in/ns/ext/atom/util.h>
// #include <lv2/lv2plug.in/ns/ext/midi/midi.h>
// #include <lv2/lv2plug.in/ns/lv2core/lv2.h>

#include <math.h>
#include <sndfile.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define LILV_LOG_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#else
#define LILV_LOG_FUNC(fmt, arg1)
#endif

#define SAMPLE_RATE 44100

/** Control port value set from the command line */
typedef struct Param
{
  const char *sym; ///< Port symbol
  float value;     ///< Control value
} Param;

/** Port type (only float ports are supported) */
typedef enum
{
  TYPE_CONTROL,
  TYPE_AUDIO
} PortType;

/** Runtime port information */
typedef struct
{
  const LilvPort *lilv_port; ///< Port description
  PortType type;             ///< Datatype
  uint32_t index;            ///< Port index
  float value;               ///< Control value (if applicable)
  bool is_input;             ///< True iff an input port
  bool optional;             ///< True iff connection optional
} Port;

/** Application state */
typedef struct
{
  LilvWorld *world;
  const LilvPlugin *plugin;
  LilvInstance *instance;
  const char *in_path;
  const char *out_path;
  SNDFILE *in_file;
  SNDFILE *out_file;
  unsigned n_params;
  Param *params;
  unsigned n_ports;
  unsigned n_audio_in;
  unsigned n_audio_out;
  Port *ports;
} LV2Apply;

static int
fatal(LV2Apply *self, int status, const char *fmt, ...);

/** Open a sound file with error handling. */
static SNDFILE *
sopen(LV2Apply *self, const char *path, int mode, SF_INFO *fmt)
{
  SNDFILE *file = sf_open(path, mode, fmt);
  const int st = sf_error(file);
  if (st)
  {
    fatal(self, 1, "Failed to open %s (%s)\n", path, sf_error_number(st));
    return NULL;
  }
  return file;
}

/** Close a sound file with error handling. */
static void
sclose(const char *path, SNDFILE *file)
{
  int st = 0;
  if (file && (st = sf_close(file)))
  {
    fatal(NULL, 1, "Failed to close %s (%s)\n", path, sf_error_number(st));
  }
}

/** Clean up all resources. */
static int
cleanup(int status, LV2Apply *self)
{
  sclose(self->out_path, self->out_file);
  lilv_instance_free(self->instance);
  lilv_world_free(self->world);
  free(self->ports);
  free(self->params);
  return status;
}

/** Print a fatal error and clean up for exit. */
LILV_LOG_FUNC(3, 4)
static int
fatal(LV2Apply *self, int status, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  fprintf(stderr, "error: ");
  vfprintf(stderr, fmt, args);
  va_end(args);
  return self ? cleanup(status, self) : status;
}

/**
   Create port structures from data (via create_port()) for all ports.
*/
static int
create_ports(LV2Apply *self)
{
  LilvWorld *world = self->world;
  const uint32_t n_ports = lilv_plugin_get_num_ports(self->plugin);

  self->n_ports = n_ports;
  self->ports = (Port *)calloc(self->n_ports, sizeof(Port));

  /* Get default values for all ports */
  float *values = (float *)calloc(n_ports, sizeof(float));
  lilv_plugin_get_port_ranges_float(self->plugin, NULL, NULL, values);

  LilvNode *lv2_InputPort = lilv_new_uri(world, LV2_CORE__InputPort);
  LilvNode *lv2_OutputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
  LilvNode *lv2_AudioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
  LilvNode *lv2_ControlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
  LilvNode *lv2_connectionOptional =
      lilv_new_uri(world, LV2_CORE__connectionOptional);

  for (uint32_t i = 0; i < n_ports; ++i)
  {
    Port *port = &self->ports[i];
    const LilvPort *lport = lilv_plugin_get_port_by_index(self->plugin, i);

    port->lilv_port = lport;
    port->index = i;
    port->value = isnan(values[i]) ? 0.0f : values[i];
    port->optional =
        lilv_port_has_property(self->plugin, lport, lv2_connectionOptional);

    /* Check if port is an input or output */
    if (lilv_port_is_a(self->plugin, lport, lv2_InputPort))
    {
      port->is_input = true;
    }
    else if (!lilv_port_is_a(self->plugin, lport, lv2_OutputPort) &&
             !port->optional)
    {
      return fatal(self, 1, "Port %u is neither input nor output\n", i);
    }

    /* Check if port is an audio or control port */
    if (lilv_port_is_a(self->plugin, lport, lv2_ControlPort))
    {
      port->type = TYPE_CONTROL;
    }
    else if (lilv_port_is_a(self->plugin, lport, lv2_AudioPort))
    {
      port->type = TYPE_AUDIO;
      if (port->is_input)
      {
        ++self->n_audio_in;
      }
      else
      {
        ++self->n_audio_out;
      }
    }
    // else if (!port->optional)
    // {
    //   return fatal(self, 1, "Port %u has unsupported type\n", i);
    // }
  }

  lilv_node_free(lv2_connectionOptional);
  lilv_node_free(lv2_ControlPort);
  lilv_node_free(lv2_AudioPort);
  lilv_node_free(lv2_OutputPort);
  lilv_node_free(lv2_InputPort);
  free(values);

  return 0;
}

// void note(LV2_Atom_Sequence *output_midi, bool on = true)
// {
//   LV2_Atom_Event event;
//   event.time.frames = 0; // frame;
//   event.body.type = 0;   // urids.midi_MidiEvent;
//   event.body.size = 3;

//   uint8_t *msg = (uint8_t *)LV2_ATOM_BODY(&event.body);

//   msg[0] = on ? LV2_MIDI_MSG_NOTE_ON : LV2_MIDI_MSG_NOTE_OFF;
//   msg[1] = 60; // plugin->last_midi_note;
//   msg[2] = 128;

//   // uint32_t capacity = output_midi->atom.size;
//   uint32_t capacity = 1;
//   lv2_atom_sequence_append_event(output_midi, capacity, &event);
// }

int main(int argc, char **argv)
{
  LV2Apply self = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, 0, 0, 0, NULL};

  /* Parse command line arguments */
  const char *plugin_uri = NULL;
  self.out_path = "out.wav";
  plugin_uri = "http://tytel.org/helm";

  /* Create world and plugin URI */
  self.world = lilv_world_new();
  LilvNode *uri = lilv_new_uri(self.world, plugin_uri);
  if (!uri)
  {
    return fatal(&self, 2, "Invalid plugin URI <%s>\n", plugin_uri);
  }

  /* Discover world */
  lilv_world_load_all(self.world);

  /* Get plugin */
  const LilvPlugins *plugins = lilv_world_get_all_plugins(self.world);
  const LilvPlugin *plugin = lilv_plugins_get_by_uri(plugins, uri);
  lilv_node_free(uri);
  if (!(self.plugin = plugin))
  {
    return fatal(&self, 3, "Plugin <%s> not found\n", plugin_uri);
  }

  /* Create port structures */
  if (create_ports(&self))
  {
    return 5;
  }

  /* Set control values */
  for (unsigned i = 0; i < self.n_params; ++i)
  {
    const Param *param = &self.params[i];
    LilvNode *sym = lilv_new_string(self.world, param->sym);
    const LilvPort *port = lilv_plugin_get_port_by_symbol(plugin, sym);
    lilv_node_free(sym);
    if (!port)
    {
      return fatal(&self, 7, "Unknown port `%s'\n", param->sym);
    }

    self.ports[lilv_port_get_index(plugin, port)].value = param->value;
  }

  /* Open output file */
  SF_INFO out_fmt = {0, 0, 0, 0, 0, 0};
  out_fmt.format = (SF_FORMAT_WAV | SF_FORMAT_PCM_24);
  out_fmt.samplerate = SAMPLE_RATE;
  out_fmt.frames = (SAMPLE_RATE * 4); /* 4 seconds */
  out_fmt.channels = self.n_audio_out;
  if (!(self.out_file = sopen(&self, self.out_path, SFM_WRITE, &out_fmt)))
  {
    free(self.ports);
    return 8;
  }

  /* Instantiate plugin and connect ports */
  const uint32_t n_ports = lilv_plugin_get_num_ports(plugin);
  float in_buf[self.n_audio_in > 0 ? self.n_audio_in : 1];
  float out_buf[self.n_audio_out > 0 ? self.n_audio_out : 1];
  self.instance = lilv_plugin_instantiate(self.plugin, SAMPLE_RATE, NULL);
  for (uint32_t p = 0, i = 0, o = 0; p < n_ports; ++p)
  {
    if (self.ports[p].type == TYPE_CONTROL)
    {
      lilv_instance_connect_port(self.instance, p, &self.ports[p].value);
    }
    else if (self.ports[p].type == TYPE_AUDIO)
    {
      if (self.ports[p].is_input)
      {
        lilv_instance_connect_port(self.instance, p, in_buf + i++);
      }
      else
      {
        lilv_instance_connect_port(self.instance, p, out_buf + o++);
      }
    }
    else
    {
      lilv_instance_connect_port(self.instance, p, NULL);
    }
  }

// maybe https://github.com/lv2/lilv/issues/26
// https://github.com/drobilla/jalv/blob/master/src/lv2_evbuf.c#L154

  // LV2_Atom_Sequence *output_midi;
  // lilv_instance_connect_port(self.instance, 0, &output_midi);
  // printf("output_midi  atom size: %d\n", output_midi->atom.size); 

  /* Ports are now connected to buffers in interleaved format, so we can run
     a single frame at a time and avoid having to interleave buffers to
     read/write from/to sndfile. */

  lilv_instance_activate(self.instance);

  // note(output_midi, true);

  // 1000 samples
  for (int64_t i = 0; i < out_fmt.frames; ++i)
  {
    lilv_instance_run(self.instance, 1);
    if (sf_writef_float(self.out_file, out_buf, 1) != 1)
    {
      return fatal(&self, 9, "Failed to write to output file\n");
    }
  }
  lilv_instance_deactivate(self.instance);

  return cleanup(0, &self);
}

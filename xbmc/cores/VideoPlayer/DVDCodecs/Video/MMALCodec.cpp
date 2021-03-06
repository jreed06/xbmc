/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined TARGET_WINDOWS)
  #include "config.h"
#elif defined(TARGET_WINDOWS)
#include "system.h"
#endif

#include "MMALCodec.h"

#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "windowing/WindowingFactory.h"
#include "cores/VideoPlayer/DVDCodecs/DVDCodecs.h"
#include "DVDVideoCodec.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "settings/Settings.h"
#include "settings/MediaSettings.h"
#include "messaging/ApplicationMessenger.h"
#include "Application.h"
#include "threads/Atomics.h"
#include "guilib/GUIWindowManager.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "settings/DisplaySettings.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/MMALRenderer.h"
#include "settings/AdvancedSettings.h"

#include "linux/RBP.h"

using namespace KODI::MESSAGING;

#define CLASSNAME "CMMALVideoBuffer"

CMMALVideoBuffer::CMMALVideoBuffer(CMMALVideo *omv)
    : m_omv(omv)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  mmal_buffer = NULL;
  m_width = 0;
  m_height = 0;
  m_aligned_width = 0;
  m_aligned_height = 0;
  m_aspect_ratio = 0.0f;
  m_refs = 0;
}

CMMALVideoBuffer::~CMMALVideoBuffer()
{
  if (mmal_buffer)
    mmal_buffer_header_release(mmal_buffer);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
}

#undef CLASSNAME
#define CLASSNAME "CMMALVideo"

CMMALVideo::CMMALVideo(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);

  m_decoded_width = 0;
  m_decoded_height = 0;
  m_decoded_aligned_width = 0;
  m_decoded_aligned_height = 0;

  m_finished = false;
  m_pFormatName = "mmal-xxxx";

  m_interlace_mode = MMAL_InterlaceProgressive;
  m_interlace_method = VS_INTERLACEMETHOD_NONE;
  m_decoderPts = DVD_NOPTS_VALUE;
  m_demuxerPts = DVD_NOPTS_VALUE;

  m_dec = NULL;
  m_dec_input = NULL;
  m_dec_output = NULL;
  m_dec_input_pool = NULL;
  m_renderer = NULL;

  m_deint = NULL;
  m_deint_connection = NULL;

  m_codingType = 0;

  m_es_format = mmal_format_alloc();
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_fps = 0.0f;
  m_num_decoded = 0;
  m_codecControlFlags = 0;
}

CMMALVideo::~CMMALVideo()
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, this);
  if (!m_finished)
    Dispose();

  CSingleLock lock(m_sharedSection);

  if (m_deint && m_deint->control && m_deint->control->is_enabled)
    mmal_port_disable(m_deint->control);

  if (m_dec && m_dec->control && m_dec->control->is_enabled)
    mmal_port_disable(m_dec->control);

  if (m_dec_input && m_dec_input->is_enabled)
    mmal_port_disable(m_dec_input);

  if (m_dec_output && m_dec_output->is_enabled)
    mmal_port_disable(m_dec_output);
  m_dec_output = NULL;

  if (m_deint_connection)
    mmal_connection_destroy(m_deint_connection);
  m_deint_connection = NULL;

  if (m_deint && m_deint->is_enabled)
    mmal_component_disable(m_deint);

  if (m_dec && m_dec->is_enabled)
      mmal_component_disable(m_dec);

  if (m_dec_input_pool)
    mmal_port_pool_destroy(m_dec_input, m_dec_input_pool);
  m_dec_input_pool = NULL;
  m_dec_input = NULL;

  if (m_deint)
    mmal_component_destroy(m_deint);
  m_deint = NULL;

  if (m_dec)
    mmal_component_destroy(m_dec);
  m_dec = NULL;
  mmal_format_free(m_es_format);
  m_es_format = NULL;
}

void CMMALVideo::PortSettingsChanged(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CSingleLock lock(m_sharedSection);
  MMAL_EVENT_FORMAT_CHANGED_T *fmt = mmal_event_format_changed_get(buffer);
  mmal_format_copy(m_es_format, fmt->format);

  if (m_es_format->es->video.crop.width && m_es_format->es->video.crop.height)
  {
    if (m_es_format->es->video.par.num && m_es_format->es->video.par.den)
      m_aspect_ratio = (float)(m_es_format->es->video.par.num * m_es_format->es->video.crop.width) / (m_es_format->es->video.par.den * m_es_format->es->video.crop.height);
    m_decoded_width = m_es_format->es->video.crop.width;
    m_decoded_height = m_es_format->es->video.crop.height;
    m_decoded_aligned_width = m_es_format->es->video.width;
    m_decoded_aligned_height = m_es_format->es->video.height;
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s::%s format changed: %dx%d (%dx%d) %.2f", CLASSNAME, __func__, m_decoded_width, m_decoded_height, m_decoded_aligned_width, m_decoded_aligned_height, m_aspect_ratio);
  }
  else
    CLog::Log(LOGERROR, "%s::%s format changed: Unexpected %dx%d (%dx%d)", CLASSNAME, __func__, m_es_format->es->video.crop.width, m_es_format->es->video.crop.height, m_decoded_aligned_width, m_decoded_aligned_height);

  if (!change_dec_output_format())
    CLog::Log(LOGERROR, "%s::%s - change_dec_output_format() failed", CLASSNAME, __func__);
}

void CMMALVideo::dec_control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  MMAL_STATUS_T status;

  if (buffer->cmd == MMAL_EVENT_ERROR)
  {
    status = (MMAL_STATUS_T)*(uint32_t *)buffer->data;
    CLog::Log(LOGERROR, "%s::%s Error (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
  }
  else if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s::%s format changed", CLASSNAME, __func__);
    PortSettingsChanged(port, buffer);
  }
  else
    CLog::Log(LOGERROR, "%s::%s other (cmd:%x data:%x)", CLASSNAME, __func__, buffer->cmd, *(uint32_t *)buffer->data);

  mmal_buffer_header_release(buffer);
}

static void dec_control_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALVideo *mmal = reinterpret_cast<CMMALVideo*>(port->userdata);
  mmal->dec_control_port_cb(port, buffer);
}


void CMMALVideo::dec_input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);
  mmal_buffer_header_release(buffer);
}

static void dec_input_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALVideo *mmal = reinterpret_cast<CMMALVideo*>(port->userdata);
  mmal->dec_input_port_cb(port, buffer);
}


void CMMALVideo::dec_output_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  if (!(buffer->cmd == 0 && buffer->length > 0))
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s::%s port:%p buffer %p, len %d cmd:%x", CLASSNAME, __func__, port, buffer, buffer->length, buffer->cmd);

  bool kept = false;

  assert(!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED));
  if (buffer->cmd == 0)
  {
    if (buffer->length > 0)
    {
      if (buffer->pts != MMAL_TIME_UNKNOWN)
        m_decoderPts = buffer->pts;
      else if (buffer->dts != MMAL_TIME_UNKNOWN)
        m_decoderPts = buffer->dts;

      assert(!(buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));
      CMMALVideoBuffer *omvb = NULL;
      bool wanted = true;
      // we don't keep up when running at 60fps in the background so switch to half rate
      if (m_fps > 40.0f && !g_graphicsContext.IsFullScreenVideo() && !(m_num_decoded & 1))
        wanted = false;
      if (g_advancedSettings.m_omxDecodeStartWithValidFrame && (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CORRUPTED))
        wanted = false;
      if (wanted)
        omvb = new CMMALVideoBuffer(this);
      m_num_decoded++;
      if (g_advancedSettings.CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG, "%s::%s - %p (%p) buffer_size(%u) dts:%.3f pts:%.3f flags:%x:%x",
          CLASSNAME, __func__, buffer, omvb, buffer->length, buffer->dts*1e-6, buffer->pts*1e-6, buffer->flags, buffer->type->video.flags);
      if (omvb)
      {
        omvb->mmal_buffer = buffer;
        buffer->user_data = (void *)omvb;
        omvb->m_width = m_decoded_width;
        omvb->m_height = m_decoded_height;
        omvb->m_aligned_width = m_decoded_aligned_width;
        omvb->m_aligned_height = m_decoded_aligned_height;
        omvb->m_aspect_ratio = m_aspect_ratio;
        {
          CSingleLock lock(m_output_mutex);
          m_output_ready.push(omvb);
          m_output_cond.notifyAll();
        }
        kept = true;
      }
    }
  }
  else if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED)
  {
    PortSettingsChanged(port, buffer);
  }
  if (!kept)
    mmal_buffer_header_release(buffer);
}

static void dec_output_port_cb_static(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
  CMMALVideo *mmal = reinterpret_cast<CMMALVideo*>(port->userdata);
  mmal->dec_output_port_cb(port, buffer);
}

bool CMMALVideo::change_dec_output_format()
{
  CSingleLock lock(m_sharedSection);
  MMAL_STATUS_T status;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  MMAL_PARAMETER_VIDEO_INTERLACE_TYPE_T interlace_type = {{ MMAL_PARAMETER_VIDEO_INTERLACE_TYPE, sizeof( interlace_type )}};
  status = mmal_port_parameter_get( m_dec_output, &interlace_type.hdr );

  if (status == MMAL_SUCCESS)
  {
    if (m_interlace_mode != interlace_type.eMode)
    {
      if (g_advancedSettings.CanLogComponent(LOGVIDEO))
        CLog::Log(LOGDEBUG, "%s::%s Interlace mode %d->%d", CLASSNAME, __func__, m_interlace_mode, interlace_type.eMode);
      m_interlace_mode = interlace_type.eMode;
    }
  }
  else
    CLog::Log(LOGERROR, "%s::%s Failed to query interlace type on %s (status=%x %s)", CLASSNAME, __func__, m_dec_output->name, status, mmal_status_to_string(status));

  mmal_format_copy(m_dec_output->format, m_es_format);

  status = mmal_port_parameter_set_boolean(m_dec_output, MMAL_PARAMETER_ZERO_COPY,  MMAL_TRUE);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, m_dec_output->name, status, mmal_status_to_string(status));

  status = mmal_port_format_commit(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  return true;
}

bool CMMALVideo::CreateDeinterlace(EINTERLACEMETHOD interlace_method)
{
  CSingleLock lock(m_sharedSection);
  MMAL_STATUS_T status;

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s method:%d", CLASSNAME, __func__, interlace_method);

  assert(!m_deint);
  assert(m_dec_output == m_dec->output[0]);

  status = mmal_port_disable(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  /* Create deinterlace filter */
  status = mmal_component_create("vc.ril.image_fx", &m_deint);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create deinterlace component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  bool advanced_deinterlace = interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED || interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF;
  bool half_framerate = interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF || interlace_method == VS_INTERLACEMETHOD_MMAL_BOB_HALF;

  MMAL_PARAMETER_IMAGEFX_PARAMETERS_T imfx_param = {{MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS, sizeof(imfx_param)},
        advanced_deinterlace ? MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV : MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST, 4, {3, 0, half_framerate, 1 }};

  status = mmal_port_parameter_set(m_deint->output[0], &imfx_param.hdr);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to set deinterlace parameters (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  MMAL_PORT_T *m_deint_input = m_deint->input[0];
  m_deint_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;

  // Image_fx assumed 3 frames of context. simple deinterlace doesn't require this
  status = mmal_port_parameter_set_uint32(m_deint_input, MMAL_PARAMETER_EXTRA_BUFFERS, GetAllowedReferences() - 5 + advanced_deinterlace ? 2:0);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable extra buffers on %s (status=%x %s)", CLASSNAME, __func__, m_deint_input->name, status, mmal_status_to_string(status));

  // Now connect the decoder output port to deinterlace input port
  status =  mmal_connection_create(&m_deint_connection, m_dec->output[0], m_deint->input[0], MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to connect deinterlacer component %s (status=%x %s)", CLASSNAME, __func__, m_deint->name, status, mmal_status_to_string(status));
    return false;
  }

  status =  mmal_connection_enable(m_deint_connection);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable connection %s (status=%x %s)", CLASSNAME, __func__, m_deint->name, status, mmal_status_to_string(status));
    return false;
  }

  mmal_format_copy(m_deint->output[0]->format, m_es_format);

  status = mmal_port_parameter_set_boolean(m_deint->output[0], MMAL_PARAMETER_ZERO_COPY,  MMAL_TRUE);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, m_deint->output[0]->name, status, mmal_status_to_string(status));

  status = mmal_port_format_commit(m_deint->output[0]);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit deint output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_enable(m_deint);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable deinterlacer component %s (status=%x %s)", CLASSNAME, __func__, m_deint->name, status, mmal_status_to_string(status));
    return false;
  }

  m_deint->output[0]->buffer_size = m_deint->output[0]->buffer_size_min;
  m_deint->output[0]->buffer_num = m_deint->output[0]->buffer_num_recommended;
  m_deint->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_deint->output[0], dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output = m_deint->output[0];
  m_interlace_method = interlace_method;
  Prime();
  return true;
}

bool CMMALVideo::DestroyDeinterlace()
{
  CSingleLock lock(m_sharedSection);
  MMAL_STATUS_T status;

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  assert(m_deint);
  assert(m_dec_output == m_deint->output[0]);

  status = mmal_port_disable(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_connection_destroy(m_deint_connection);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to destroy deinterlace connection (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  m_deint_connection = NULL;

  status = mmal_component_disable(m_deint);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to disable deinterlace component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_destroy(m_deint);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to destroy deinterlace component (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  m_deint = NULL;

  m_dec->output[0]->buffer_size = m_dec->output[0]->buffer_size_min;
  m_dec->output[0]->buffer_num = m_dec->output[0]->buffer_num_recommended;
  m_dec->output[0]->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec->output[0], dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output = m_dec->output[0];
  m_interlace_method = VS_INTERLACEMETHOD_NONE;
  Prime();
  return true;
}

bool CMMALVideo::SendCodecConfigData()
{
  CSingleLock lock(m_sharedSection);
  MMAL_STATUS_T status;
  if (!m_dec_input_pool || !m_hints.extrasize)
    return true;
  // send code config data
  MMAL_BUFFER_HEADER_T *buffer = mmal_queue_timedwait(m_dec_input_pool->queue, 500);
  if (!buffer)
  {
    CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __func__);
    return false;
  }

  mmal_buffer_header_reset(buffer);
  buffer->cmd = 0;
  buffer->length = std::min(m_hints.extrasize, buffer->alloc_size);
  memcpy(buffer->data, m_hints.extradata, buffer->length);
  buffer->flags = MMAL_BUFFER_HEADER_FLAG_FRAME_END | MMAL_BUFFER_HEADER_FLAG_CONFIG;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d flags:%x", CLASSNAME, __func__, buffer, buffer->length, buffer->flags);
  status = mmal_port_send_buffer(m_dec_input, buffer);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed send buffer to decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }
  return true;
}

bool CMMALVideo::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  CSingleLock lock(m_sharedSection);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s usemmal:%d software:%d %dx%d renderer:%p", CLASSNAME, __func__, CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMMAL), hints.software, hints.width, hints.height, options.m_opaque_pointer);

  // we always qualify even if DVDFactoryCodec does this too.
  if (!CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMMAL) || hints.software)
    return false;

  m_hints = hints;
  m_renderer = (CMMALRenderer *)options.m_opaque_pointer;
  MMAL_STATUS_T status;

  m_decoded_width = m_decoded_aligned_width = hints.width;
  m_decoded_height = m_decoded_aligned_height = hints.height;

  // use aspect in stream if available
  if (m_hints.forced_aspect)
    m_aspect_ratio = m_hints.aspect;
  else
    m_aspect_ratio = 0.0;

  switch (hints.codec)
  {
    case AV_CODEC_ID_H264:
      // H.264
      m_codingType = MMAL_ENCODING_H264;
      m_pFormatName = "mmal-h264";
      if (CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_SUPPORTMVC))
      {
        m_codingType = MMAL_ENCODING_MVC;
        m_pFormatName= "mmal-mvc";
      }
    break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_MPEG4:
      // MPEG-4, DivX 4/5 and Xvid compatible
      m_codingType = MMAL_ENCODING_MP4V;
      m_pFormatName = "mmal-mpeg4";
    break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // MPEG-2
      m_codingType = MMAL_ENCODING_MP2V;
      m_pFormatName = "mmal-mpeg2";
    break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // VP6
      m_codingType = MMAL_ENCODING_VP6;
      m_pFormatName = "mmal-vp6";
    break;
    case AV_CODEC_ID_VP8:
      // VP8
      m_codingType = MMAL_ENCODING_VP8;
      m_pFormatName = "mmal-vp8";
    break;
    case AV_CODEC_ID_THEORA:
      // theora
      m_codingType = MMAL_ENCODING_THEORA;
      m_pFormatName = "mmal-theora";
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // mjpg
      m_codingType = MMAL_ENCODING_MJPEG;
      m_pFormatName = "mmal-mjpg";
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // VC-1, WMV9
      m_codingType = MMAL_ENCODING_WVC1;
      m_pFormatName = "mmal-vc1";
      break;
    default:
      CLog::Log(LOGERROR, "%s::%s : Video codec unknown: %x", CLASSNAME, __func__, hints.codec);
      return false;
    break;
  }

  if ( (m_codingType == MMAL_ENCODING_MP2V && !g_RBP.GetCodecMpg2() ) ||
       (m_codingType == MMAL_ENCODING_WVC1 && !g_RBP.GetCodecWvc1() ) )
  {
    CLog::Log(LOGWARNING, "%s::%s Codec %s is not supported", CLASSNAME, __func__, m_pFormatName);
    return false;
  }

  // initialize mmal.
  status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, &m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create MMAL decoder component %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec->control->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec->control, dec_control_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder control port %s (status=%x %s)", CLASSNAME, __func__, MMAL_COMPONENT_DEFAULT_VIDEO_DECODER, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_input = m_dec->input[0];

  m_dec_input->format->type = MMAL_ES_TYPE_VIDEO;
  m_dec_input->format->encoding = m_codingType;
  if (m_hints.width && m_hints.height)
  {
    m_dec_input->format->es->video.crop.width = m_hints.width;
    m_dec_input->format->es->video.crop.height = m_hints.height;

    m_dec_input->format->es->video.width = ALIGN_UP(m_hints.width, 32);
    m_dec_input->format->es->video.height = ALIGN_UP(m_hints.height, 16);
  }
  if (hints.fpsrate > 0 && hints.fpsscale > 0)
  {
    m_dec_input->format->es->video.frame_rate.num = hints.fpsrate;
    m_dec_input->format->es->video.frame_rate.den = hints.fpsscale;
    m_fps = hints.fpsrate / hints.fpsscale;
  }
  else
    m_fps = 0.0f;
  m_dec_input->format->flags |= MMAL_ES_FORMAT_FLAG_FRAMED;

  status = mmal_port_parameter_set_boolean(m_dec_input, MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT, MMAL_FALSE);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to disable error concealment on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_port_parameter_set_uint32(m_dec_input, MMAL_PARAMETER_EXTRA_BUFFERS, GetAllowedReferences());
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable extra buffers on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_port_parameter_set_uint32(m_dec_input, MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, 1);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to disable interpolate timestamps mode on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  // limit number of callback structures in video_decode to reduce latency. Too low and video hangs.
  // negative numbers have special meaning. -1=size of DPB -2=size of DPB+1
  status = mmal_port_parameter_set_uint32(m_dec_input, MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS, -5);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to configure max num callbacks on %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));

  status = mmal_port_format_commit(m_dec_input);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit format for decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }
  // use a small number of large buffers to keep latency under control
  m_dec_input->buffer_size = 1024*1024;
  m_dec_input->buffer_num = 2;

  m_dec_input->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec_input, dec_input_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder input port %s (status=%x %s)", CLASSNAME, __func__, m_dec_input->name, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output = m_dec->output[0];

  // set up initial decoded frame format - will likely change from this
  m_dec_output->format->encoding = MMAL_ENCODING_OPAQUE;
  mmal_format_copy(m_es_format, m_dec_output->format);

  status = mmal_port_parameter_set_boolean(m_dec_output, MMAL_PARAMETER_ZERO_COPY,  MMAL_TRUE);
  if (status != MMAL_SUCCESS)
    CLog::Log(LOGERROR, "%s::%s Failed to enable zero copy mode on %s (status=%x %s)", CLASSNAME, __func__, m_dec_output->name, status, mmal_status_to_string(status));

  status = mmal_port_format_commit(m_dec_output);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to commit decoder output format (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_output->buffer_size = m_dec_output->buffer_size_min;
  m_dec_output->buffer_num = m_dec_output->buffer_num_recommended;
  m_dec_output->userdata = (struct MMAL_PORT_USERDATA_T *)this;
  status = mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder output port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  status = mmal_component_enable(m_dec);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to enable decoder component %s (status=%x %s)", CLASSNAME, __func__, m_dec->name, status, mmal_status_to_string(status));
    return false;
  }

  m_dec_input_pool = mmal_port_pool_create(m_dec_input, m_dec_input->buffer_num, m_dec_input->buffer_size);
  if (!m_dec_input_pool)
  {
    CLog::Log(LOGERROR, "%s::%s Failed to create pool for decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return false;
  }

  if (!SendCodecConfigData())
    return false;

  Prime();
  m_speed = DVD_PLAYSPEED_NORMAL;

  return true;
}

void CMMALVideo::Dispose()
{
  CSingleLock lock(m_sharedSection);
  m_finished = true;
  Reset();
}

void CMMALVideo::SetDropState(bool bDrop)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - bDrop(%d)", CLASSNAME, __func__, bDrop);
  m_dropState = bDrop;
}

int CMMALVideo::Decode(uint8_t* pData, int iSize, double dts, double pts)
{
  CSingleLock lock(m_sharedSection);
  //if (g_advancedSettings.CanLogComponent(LOGVIDEO))
  //  CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d dts:%.3f pts:%.3f ready_queue(%d)",
  //    CLASSNAME, __func__, pData, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, m_output_ready.size());

  MMAL_BUFFER_HEADER_T *buffer;
  MMAL_STATUS_T status;

  Prime();
  while (1)
  {
     if (pData)
     {
       // 500ms timeout
       {
         CSingleExit unlock(m_sharedSection);
         buffer = mmal_queue_timedwait(m_dec_input_pool->queue, 500);
         if (!buffer)
         {
           CLog::Log(LOGERROR, "%s::%s - mmal_queue_get failed", CLASSNAME, __func__);
           return VC_ERROR;
         }
       }
       mmal_buffer_header_reset(buffer);
       buffer->cmd = 0;
       buffer->pts = pts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : pts;
       buffer->dts = dts == DVD_NOPTS_VALUE ? MMAL_TIME_UNKNOWN : dts;
       if (m_hints.ptsinvalid) buffer->pts = MMAL_TIME_UNKNOWN;
       buffer->length = (uint32_t)iSize > buffer->alloc_size ? buffer->alloc_size : (uint32_t)iSize;
       // set a flag so we can identify primary frames from generated frames (deinterlace)
       buffer->flags = MMAL_BUFFER_HEADER_FLAG_USER0;
       if (m_dropState)
         buffer->flags |= MMAL_BUFFER_HEADER_FLAG_USER3;

       memcpy(buffer->data, pData, buffer->length);
       iSize -= buffer->length;
       pData += buffer->length;

       if (iSize == 0)
         buffer->flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_END;

       if (g_advancedSettings.CanLogComponent(LOGVIDEO))
         CLog::Log(LOGDEBUG, "%s::%s - %-8p %-6d/%-6d dts:%.3f pts:%.3f flags:%x ready_queue(%d)",
            CLASSNAME, __func__, buffer, buffer->length, iSize, dts == DVD_NOPTS_VALUE ? 0.0 : dts*1e-6, pts == DVD_NOPTS_VALUE ? 0.0 : pts*1e-6, buffer->flags, m_output_ready.size());
       assert((int)buffer->length > 0);
       status = mmal_port_send_buffer(m_dec_input, buffer);
       if (status != MMAL_SUCCESS)
       {
         CLog::Log(LOGERROR, "%s::%s Failed send buffer to decoder input port (status=%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
         return VC_ERROR;
       }

       if (iSize == 0)
       {
         EDEINTERLACEMODE deinterlace_request = CMediaSettings::GetInstance().GetCurrentVideoSettings().m_DeinterlaceMode;
         EINTERLACEMETHOD interlace_method = CMediaSettings::GetInstance().GetCurrentVideoSettings().m_InterlaceMethod;
         if (interlace_method == VS_INTERLACEMETHOD_AUTO)
           interlace_method = VS_INTERLACEMETHOD_MMAL_ADVANCED;
         bool deinterlace = m_interlace_mode != MMAL_InterlaceProgressive;

         // we don't keep up when running at 60fps in the background so switch to half rate
         if (!g_graphicsContext.IsFullScreenVideo())
         {
           if (interlace_method == VS_INTERLACEMETHOD_MMAL_ADVANCED)
             interlace_method = VS_INTERLACEMETHOD_MMAL_ADVANCED_HALF;
           if (interlace_method == VS_INTERLACEMETHOD_MMAL_BOB)
             interlace_method = VS_INTERLACEMETHOD_MMAL_BOB_HALF;
         }

         if (m_hints.stills || deinterlace_request == VS_DEINTERLACEMODE_OFF || interlace_method == VS_INTERLACEMETHOD_NONE)
           deinterlace = false;
         else if (deinterlace_request == VS_DEINTERLACEMODE_FORCE)
           deinterlace = true;

         if (((deinterlace && interlace_method != m_interlace_method) || !deinterlace) && m_deint)
           DestroyDeinterlace();
         if (deinterlace && !m_deint)
           CreateDeinterlace(interlace_method);
       }
    }
    if (!iSize)
      break;
  }
  if (pts != DVD_NOPTS_VALUE)
    m_demuxerPts = pts;
  else if (dts != DVD_NOPTS_VALUE)
    m_demuxerPts = dts;

  if (m_demuxerPts != DVD_NOPTS_VALUE && m_decoderPts == DVD_NOPTS_VALUE)
    m_decoderPts = m_demuxerPts;

  // we've built up quite a lot of data in decoder - try to throttle it
  double queued = m_decoderPts != DVD_NOPTS_VALUE && m_demuxerPts != DVD_NOPTS_VALUE ? m_demuxerPts - m_decoderPts : 0.0;
  bool full = queued > DVD_MSEC_TO_TIME(1000);
  int ret = 0;

  if (!m_output_ready.empty())
    ret |= VC_PICTURE;
  if (mmal_queue_length(m_dec_input_pool->queue) > 0 && !(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
    ret |= VC_BUFFER;

  bool slept = false;
  if (!ret)
  {
    slept = true;
    {
      // otherwise we busy spin
      CSingleExit unlock(m_sharedSection);
      CSingleLock lock(m_output_mutex);
      m_output_cond.wait(lock, 30);
    }
    if (!m_output_ready.empty())
      ret |= VC_PICTURE;
    if (mmal_queue_length(m_dec_input_pool->queue) > 0 && !(m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
      ret |= VC_BUFFER;
    else if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN && !ret)
      ret |= VC_BUFFER;
  }

  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - ret(%x) pics(%d) inputs(%d) slept(%d) queued(%.2f) (%.2f:%.2f) full(%d) flags(%x)", CLASSNAME, __func__, ret, m_output_ready.size(), mmal_queue_length(m_dec_input_pool->queue), slept, queued*1e-6, m_demuxerPts*1e-6, m_decoderPts*1e-6, full, m_codecControlFlags);

  return ret;
}

void CMMALVideo::Prime()
{
  MMAL_BUFFER_HEADER_T *buffer;
  assert(m_renderer);
  MMAL_POOL_T *render_pool = m_renderer->GetPool(RENDER_FMT_MMAL, true);
  assert(render_pool);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s - queue(%p)", CLASSNAME, __func__, render_pool);
  while (buffer = mmal_queue_get(render_pool->queue), buffer)
    Recycle(buffer);
}

void CMMALVideo::Reset(void)
{
  CSingleLock lock(m_sharedSection);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s", CLASSNAME, __func__);

  if (m_dec_input && m_dec_input->is_enabled)
    mmal_port_disable(m_dec_input);
  if (m_deint_connection && m_deint_connection->is_enabled)
    mmal_connection_disable(m_deint_connection);
  if (m_dec_output && m_dec_output->is_enabled)
    mmal_port_disable(m_dec_output);
  if (!m_finished)
  {
    if (m_dec_input)
      mmal_port_enable(m_dec_input, dec_input_port_cb_static);
    if (m_deint_connection)
      mmal_connection_enable(m_deint_connection);
    if (m_dec_output)
      mmal_port_enable(m_dec_output, dec_output_port_cb_static);
  }
  // blow all ready video frames
  while (1)
  {
    CMMALVideoBuffer *buffer = NULL;
    {
      CSingleLock lock(m_output_mutex);
      // fetch a output buffer and pop it off the ready list
      if (!m_output_ready.empty())
      {
        buffer = m_output_ready.front();
        m_output_ready.pop();
      }
      m_output_cond.notifyAll();
    }
    if (buffer)
    {
      buffer->Acquire();
      buffer->Release();
    }
    else
      break;
  }

  if (!m_finished)
  {
    SendCodecConfigData();
    Prime();
  }
  m_decoderPts = DVD_NOPTS_VALUE;
  m_demuxerPts = DVD_NOPTS_VALUE;
  m_codecControlFlags = 0;
  m_dropState = false;
}

void CMMALVideo::SetSpeed(int iSpeed)
{
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %d->%d", CLASSNAME, __func__, m_speed, iSpeed);

  m_speed = iSpeed;
}

void CMMALVideo::Recycle(MMAL_BUFFER_HEADER_T *buffer)
{
  CSingleLock lock(m_sharedSection);
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s %p", CLASSNAME, __func__, buffer);

  if (m_finished)
  {
    mmal_buffer_header_release(buffer);
    return;
  }

  MMAL_STATUS_T status;
  mmal_buffer_header_reset(buffer);
  buffer->cmd = 0;
  if (g_advancedSettings.CanLogComponent(LOGVIDEO))
    CLog::Log(LOGDEBUG, "%s::%s Send buffer %p from pool to decoder output port %p ready_queue(%d)", CLASSNAME, __func__, buffer, m_dec_output,
      m_output_ready.size());
  status = mmal_port_send_buffer(m_dec_output, buffer);
  if (status != MMAL_SUCCESS)
  {
    CLog::Log(LOGERROR, "%s::%s - Failed send buffer to decoder output port (status=0%x %s)", CLASSNAME, __func__, status, mmal_status_to_string(status));
    return;
  }
}

bool CMMALVideo::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  CSingleLock lock(m_sharedSection);
  if (!m_output_ready.empty())
  {
    CMMALVideoBuffer *buffer;
    // fetch a output buffer and pop it off the ready list
    {
      CSingleLock lock(m_output_mutex);
      buffer = m_output_ready.front();
      m_output_ready.pop();
      m_output_cond.notifyAll();
    }
    assert(buffer->mmal_buffer);
    memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
    pDvdVideoPicture->format = RENDER_FMT_MMAL;
    pDvdVideoPicture->MMALBuffer = buffer;
    pDvdVideoPicture->color_range  = 0;
    pDvdVideoPicture->color_matrix = 4;
    pDvdVideoPicture->iWidth  = buffer->m_width ? buffer->m_width : m_decoded_width;
    pDvdVideoPicture->iHeight = buffer->m_height ? buffer->m_height : m_decoded_height;
    pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
    pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;
    //CLog::Log(LOGDEBUG, "%s::%s -  %dx%d %dx%d %dx%d %dx%d %f,%f", CLASSNAME, __func__, pDvdVideoPicture->iWidth, pDvdVideoPicture->iHeight, pDvdVideoPicture->iDisplayWidth, pDvdVideoPicture->iDisplayHeight, m_decoded_width, m_decoded_height, buffer->m_width, buffer->m_height, buffer->m_aspect_ratio, m_hints.aspect);

    if (buffer->m_aspect_ratio > 0.0)
    {
      pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * buffer->m_aspect_ratio)) & -3;
      if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
      {
        pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
        pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / buffer->m_aspect_ratio)) & -3;
      }
    }

    // timestamp is in microseconds
    pDvdVideoPicture->dts = buffer->mmal_buffer->dts == MMAL_TIME_UNKNOWN || buffer->mmal_buffer->dts == 0 ? DVD_NOPTS_VALUE : buffer->mmal_buffer->dts;
    pDvdVideoPicture->pts = buffer->mmal_buffer->pts == MMAL_TIME_UNKNOWN || buffer->mmal_buffer->pts == 0 ? DVD_NOPTS_VALUE : buffer->mmal_buffer->pts;

    pDvdVideoPicture->MMALBuffer->Acquire();
    pDvdVideoPicture->iFlags  = DVP_FLAG_ALLOCATED;
    if (buffer->mmal_buffer->flags & MMAL_BUFFER_HEADER_FLAG_USER3)
      pDvdVideoPicture->iFlags |= DVP_FLAG_DROPPED;
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGINFO, "%s::%s dts:%.3f pts:%.3f flags:%x:%x MMALBuffer:%p mmal_buffer:%p", CLASSNAME, __func__,
          pDvdVideoPicture->dts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->dts*1e-6, pDvdVideoPicture->pts == DVD_NOPTS_VALUE ? 0.0 : pDvdVideoPicture->pts*1e-6,
          pDvdVideoPicture->iFlags, buffer->mmal_buffer->flags, pDvdVideoPicture->MMALBuffer, pDvdVideoPicture->MMALBuffer->mmal_buffer);
    assert(!(buffer->mmal_buffer->flags & MMAL_BUFFER_HEADER_FLAG_DECODEONLY));
  }
  else
  {
    CLog::Log(LOGERROR, "%s::%s - called but m_output_ready is empty", CLASSNAME, __func__);
    return false;
  }

  return true;
}

bool CMMALVideo::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  CSingleLock lock(m_sharedSection);
  if (pDvdVideoPicture->format == RENDER_FMT_MMAL)
  {
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s::%s - %p (%p)", CLASSNAME, __func__, pDvdVideoPicture->MMALBuffer, pDvdVideoPicture->MMALBuffer->mmal_buffer);
    pDvdVideoPicture->MMALBuffer->Release();
  }
  memset(pDvdVideoPicture, 0, sizeof *pDvdVideoPicture);
  return true;
}

bool CMMALVideo::GetCodecStats(double &pts, int &droppedPics)
{
  CSingleLock lock(m_sharedSection);
  droppedPics= -1;
  return false;
}

void CMMALVideo::SetCodecControl(int flags)
{
  CSingleLock lock(m_sharedSection);
  if (m_codecControlFlags != flags)
    if (g_advancedSettings.CanLogComponent(LOGVIDEO))
      CLog::Log(LOGDEBUG, "%s::%s %x->%x", CLASSNAME, __func__, m_codecControlFlags, flags);
  m_codecControlFlags = flags;
}

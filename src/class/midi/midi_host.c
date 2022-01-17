/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if (TUSB_OPT_HOST_ENABLED && CFG_TUH_MIDI)

#include "host/usbh.h"
#include "host/usbh_classdriver.h"

#include "midi_host.h"


//extern "C" {


//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
#ifndef CFG_TUH_MAX_CABLES
  #define CFG_TUH_MAX_CABLES 16
#endif
#define CFG_TUH_MIDI_RX_BUFSIZE 64
#define CFG_TUH_MIDI_TX_BUFSIZE 64
#ifndef CFG_TUH_MIDI_EP_BUFSIZE
  #define CFG_TUH_MIDI_EP_BUFSIZE 64
#endif

// TODO: refactor to share code with the MIDI Device driver
typedef struct
{
  uint8_t buffer[4];
  uint8_t index;
  uint8_t total;
}midi_stream_t;

typedef struct
{
  uint8_t dev_addr;
  uint8_t itf_num;

  uint8_t ep_in;          // IN endpoint address
  uint8_t ep_out;         // OUT endpoint address
  uint16_t ep_in_max;     // min( CFG_TUH_MIDI_RX_BUFSIZE, wMaxPacketSize of the IN endpoint)
  uint16_t ep_out_max;    //  min( CFG_TUH_MIDI_TX_BUFSIZE, wMaxPacketSize of the OUT endpoint)

  uint8_t num_cables_rx;  // IN endpoint CS descriptor bNumEmbMIDIJack value
  uint8_t num_cables_tx;  // OUT endpoint CS descriptor bNumEmbMIDIJack value

  // For Stream read()/write() API
  // Messages are always 4 bytes long, queue them for reading and writing so the
  // callers can use the Stream interface with single-byte read/write calls.
  midi_stream_t stream_write;
  midi_stream_t stream_read;

  //------------- From this point, data is not cleared by bus reset -------------
  // Endpoint FIFOs
  tu_fifo_t rx_ff;
  tu_fifo_t tx_ff;
 

  uint8_t rx_ff_buf[CFG_TUH_MIDI_RX_BUFSIZE];
  uint8_t tx_ff_buf[CFG_TUH_MIDI_TX_BUFSIZE];

  #if CFG_FIFO_MUTEX
  osal_mutex_def_t rx_ff_mutex;
  osal_mutex_def_t tx_ff_mutex;
  #endif

  // Endpoint Transfer buffer
  CFG_TUSB_MEM_ALIGN uint8_t epout_buf[CFG_TUH_MIDI_EP_BUFSIZE];
  CFG_TUSB_MEM_ALIGN uint8_t epin_buf[CFG_TUH_MIDI_EP_BUFSIZE];

  bool configured;
}midih_interface_t;

static midih_interface_t _midi_host[CFG_TUH_DEVICE_MAX];

static midih_interface_t *get_midi_host(uint8_t dev_addr)
{
  TU_ASSERT(dev_addr >0 && dev_addr <= CFG_TUH_DEVICE_MAX);
  return (_midi_host + dev_addr - 1);
}

//------------- Internal prototypes -------------//
static uint32_t write_flush(uint8_t dev_addr, midih_interface_t* midi);

//--------------------------------------------------------------------+
// USBH API
//--------------------------------------------------------------------+
void midih_init(void)
{
  tu_memclr(&_midi_host, sizeof(_midi_host));

  // config fifos
  for (int inst = 0; inst < CFG_TUH_DEVICE_MAX; inst++)
  {
    midih_interface_t *p_midi_host = &_midi_host[inst];
    tu_fifo_config(&p_midi_host->rx_ff, p_midi_host->rx_ff_buf, CFG_TUH_MIDI_RX_BUFSIZE, 1, false); // true, true
    tu_fifo_config(&p_midi_host->tx_ff, p_midi_host->tx_ff_buf, CFG_TUH_MIDI_TX_BUFSIZE, 1, false); // OBVS.

  #if CFG_FIFO_MUTEX
    tu_fifo_config_mutex(&p_midi_host->rx_ff, NULL, osal_mutex_create(&p_midi_host->rx_ff_mutex));
    tu_fifo_config_mutex(&p_midi_host->tx_ff, osal_mutex_create(&p_midi_host->tx_ff_mutex), NULL);
  #endif
  }
}

bool midih_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  if ( ep_addr == p_midi_host->ep_in)
  {
    if (0 == xferred_bytes)
    {
      return true; // No data to handle
    }

    // receive new data if available
    uint32_t packets_queued = 0;
    if (xferred_bytes)
    {
      // put in the RX FIFO only non-zero MIDI IN 4-byte packets
      uint8_t* buf = p_midi_host->epin_buf;
      uint32_t npackets = xferred_bytes / 4;
      uint32_t packet_num;
      for (packet_num = 0; packet_num < npackets; packet_num++)
      {
        // some devices send back all zero packets even if there is no data ready
        uint32_t packet = (uint32_t)((*buf)<<24) | ((uint32_t)(*(buf+1))<<16) | ((uint32_t)(*(buf+2))<<8) | ((uint32_t)(*(buf+3)));
        if (packet != 0)
        {
          tu_fifo_write_n(&p_midi_host->rx_ff, buf, 4);
          ++packets_queued;
          TU_LOG3("MIDI RX=%08x\r\n", packet);
        }
        buf += 4;
      }
    }
    // invoke receive callback if available
    if (tuh_midi_rx_cb)
    {
      tuh_midi_rx_cb(dev_addr, packets_queued);
    }
  }
  else if ( ep_addr == p_midi_host->ep_out )
  {
    if (0 == write_flush(dev_addr, p_midi_host))
    {
      // If there is no data left, a ZLP should be sent if
      // xferred_bytes is multiple of EP size and not zero
      if ( !tu_fifo_count(&p_midi_host->tx_ff) && xferred_bytes && (0 == (xferred_bytes % p_midi_host->ep_out_max)) )
      {
        if ( usbh_edpt_claim(dev_addr, p_midi_host->ep_out) )
        {
          TU_ASSERT(usbh_edpt_xfer(dev_addr, p_midi_host->ep_out, XFER_RESULT_SUCCESS, 0));
        }
      }
    }
    if (tuh_midi_tx_cb)
    {
      tuh_midi_tx_cb(dev_addr);
    }
  }

  return true;
}

void midih_close(uint8_t dev_addr)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  if (tuh_midi_umount_cb)
    tuh_midi_umount_cb(dev_addr, 0);
  tu_fifo_clear(&p_midi_host->rx_ff);
  tu_fifo_clear(&p_midi_host->tx_ff);
  p_midi_host->ep_in = 0;
  p_midi_host->ep_in_max = 0;
  p_midi_host->ep_out = 0;
  p_midi_host->ep_out_max = 0;
  p_midi_host->itf_num = 0;
  p_midi_host->num_cables_rx = 0;
  p_midi_host->num_cables_tx = 0;
  p_midi_host->dev_addr = 255; // invalid
  p_midi_host->configured = false;
  tu_memclr(&p_midi_host->stream_read, sizeof(p_midi_host->stream_read));
  tu_memclr(&p_midi_host->stream_write, sizeof(p_midi_host->stream_write));
}

//--------------------------------------------------------------------+
// Enumeration
//--------------------------------------------------------------------+
bool midih_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *desc_itf, uint16_t max_len)
{
  (void) rhport;
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  TU_VERIFY(TUSB_CLASS_AUDIO == desc_itf->bInterfaceClass);
  // There can be just a MIDI interface or an audio and a MIDI interface. Only open the MIDI interface
  uint8_t const *p_desc = (uint8_t const *) desc_itf;
  uint16_t len_parsed = 0;
  if (AUDIO_SUBCLASS_CONTROL == desc_itf->bInterfaceSubClass)
  {
    // This driver does not support audio streaming. However, if this is the audio control interface
    // there might be a MIDI interface following it. Search through every descriptor until a MIDI
    // interface is found or the end of the descriptor is found
    while (len_parsed < max_len && (desc_itf->bInterfaceClass != TUSB_CLASS_AUDIO || desc_itf->bInterfaceSubClass != AUDIO_SUBCLASS_MIDI_STREAMING))
    {
      len_parsed += desc_itf->bLength;
      p_desc = tu_desc_next(p_desc);
      desc_itf = (tusb_desc_interface_t const *)p_desc;
    }

    TU_VERIFY(TUSB_CLASS_AUDIO == desc_itf->bInterfaceClass);
  }
  TU_VERIFY(AUDIO_SUBCLASS_MIDI_STREAMING == desc_itf->bInterfaceSubClass);
  len_parsed += desc_itf->bLength;

  p_desc = tu_desc_next(p_desc);
  TU_LOG1("MIDI opening Interface %u (addr = %u)\r\n", desc_itf->bInterfaceNumber, dev_addr);
  // Find out if getting the MIDI class specific interface header or an endpoint descriptor
  // or a class-specific endpoint descriptor
  // Jack descriptors or element descriptors must follow the cs interface header,
  // but this driver does not support devices that contain element descriptors

  // assume it is an interface header
  midi_desc_header_t const *p_mdh = (midi_desc_header_t const *)p_desc;
  TU_VERIFY((p_mdh->bDescriptorType == TUSB_DESC_CS_INTERFACE && p_mdh->bDescriptorSubType == MIDI_CS_INTERFACE_HEADER) || 
    (p_mdh->bDescriptorType == TUSB_DESC_CS_ENDPOINT && p_mdh->bDescriptorSubType == MIDI_MS_ENDPOINT_GENERAL) ||
    p_mdh->bDescriptorType == TUSB_DESC_ENDPOINT);

  uint8_t prev_ep_addr = 0; // the CS endpoint descriptor is associated with the previous endpoint descrptor
  p_midi_host->itf_num = desc_itf->bInterfaceNumber;
  tusb_desc_endpoint_t const* in_desc = NULL;
  tusb_desc_endpoint_t const* out_desc = NULL;
  while (len_parsed < max_len)
  {
    TU_VERIFY((p_mdh->bDescriptorType == TUSB_DESC_CS_INTERFACE) || 
      (p_mdh->bDescriptorType == TUSB_DESC_CS_ENDPOINT && p_mdh->bDescriptorSubType == MIDI_MS_ENDPOINT_GENERAL) ||
      p_mdh->bDescriptorType == TUSB_DESC_ENDPOINT);

    if (p_mdh->bDescriptorType == TUSB_DESC_CS_INTERFACE) {
      // The USB host doesn't really need this information unless it uses
      // the string descriptor for a jack or Element

      // assume it is an input jack
      midi_desc_in_jack_t const *p_mdij = (midi_desc_in_jack_t const *)p_desc;
      if (p_mdij->bDescriptorSubType == MIDI_CS_INTERFACE_HEADER)
      {
        TU_LOG2("Found MIDI Interface Header\r\b");
      }
      else if (p_mdij->bDescriptorSubType == MIDI_CS_INTERFACE_IN_JACK)
      {
        // Then it is an in jack. 
        TU_LOG2("Found in jack\r\n");
      }
      else if (p_mdij->bDescriptorSubType == MIDI_CS_INTERFACE_OUT_JACK)
      {
        // then it is an out jack
        TU_LOG2("Found out jack\r\n");
      }
      else if (p_mdij->bDescriptorSubType == MIDI_CS_INTERFACE_ELEMENT)
      {
        // the it is an element;
        TU_LOG2("Found element\r\n");
      }
      else
      {
        TU_LOG2("Unknown CS Interface sub-type %u\r\n", p_mdij->bDescriptorSubType);
        TU_VERIFY(false); // unknown CS Interface sub-type
      }
      len_parsed += p_mdij->bLength;
    }
    else if (p_mdh->bDescriptorType == TUSB_DESC_CS_ENDPOINT)
    {
      TU_LOG2("found CS_ENDPOINT Descriptor for %u\r\n", prev_ep_addr);
      TU_VERIFY(prev_ep_addr != 0);
      // parse out the mapping between the device's embedded jacks and the endpoints
      // Each embedded IN jack is assocated with an OUT endpoint
      midi_cs_desc_endpoint_t const* p_csep = (midi_cs_desc_endpoint_t const*)p_mdh;
      if (tu_edpt_dir(prev_ep_addr) == TUSB_DIR_OUT)
      {
        TU_VERIFY(p_midi_host->ep_out == prev_ep_addr);
        TU_VERIFY(p_midi_host->num_cables_tx == 0);
        p_midi_host->num_cables_tx = p_csep->bNumEmbMIDIJack;
      }
      else
      {
        TU_VERIFY(p_midi_host->ep_in == prev_ep_addr);
        TU_VERIFY(p_midi_host->num_cables_rx == 0);
        p_midi_host->num_cables_rx = p_csep->bNumEmbMIDIJack;
      }
      len_parsed += p_csep->bLength;
      prev_ep_addr = 0;
    }
    else if (p_mdh->bDescriptorType == TUSB_DESC_ENDPOINT) {
      // parse out the bulk endpoint info
      tusb_desc_endpoint_t const *p_ep = (tusb_desc_endpoint_t const *)p_mdh;
      TU_LOG2("found ENDPOINT Descriptor for %u\r\n", p_ep->bEndpointAddress);
      if (tu_edpt_dir(p_ep->bEndpointAddress) == TUSB_DIR_OUT)
      {
        TU_VERIFY(p_midi_host->ep_out == 0);
        TU_VERIFY(p_midi_host->num_cables_tx == 0);
        p_midi_host->ep_out = p_ep->bEndpointAddress;
        p_midi_host->ep_out_max = p_ep->wMaxPacketSize;
        if (p_midi_host->ep_out_max > CFG_TUH_MIDI_TX_BUFSIZE)
          p_midi_host->ep_out_max = CFG_TUH_MIDI_TX_BUFSIZE;
        prev_ep_addr = p_midi_host->ep_out;
        out_desc = p_ep;
      }
      else
      {
        TU_VERIFY(p_midi_host->ep_in == 0);
        TU_VERIFY(p_midi_host->num_cables_rx == 0);
        p_midi_host->ep_in = p_ep->bEndpointAddress;
        p_midi_host->ep_in_max = p_ep->wMaxPacketSize;
        if (p_midi_host->ep_in_max > CFG_TUH_MIDI_RX_BUFSIZE)
          p_midi_host->ep_in_max = CFG_TUH_MIDI_RX_BUFSIZE;
        prev_ep_addr = p_midi_host->ep_in;
        in_desc = p_ep;
      }
      len_parsed += p_mdh->bLength;
    }
    p_desc = tu_desc_next(p_desc);
    p_mdh = (midi_desc_header_t const *)p_desc;
  }
  TU_VERIFY((p_midi_host->ep_out != 0 && p_midi_host->num_cables_tx != 0) ||
            (p_midi_host->ep_in != 0 && p_midi_host->num_cables_rx != 0));
  TU_LOG1("MIDI descriptor parsed successfully\r\n");

  if (in_desc)
  {
    TU_ASSERT(usbh_edpt_open(rhport, dev_addr, in_desc));
    // Some devices always return exactly the request length so transfers won't complete
    // unless you assume every transfer is the last one.
    usbh_edpt_force_last_buffer(dev_addr, p_midi_host->ep_in, true);
    // Some devices will always NAK the initial IN-ep transfer request and
    // depending on your host hardware, this will cause hanging while retrying 
    // the NAK'd IN transfer forever right after enumeration, before the mount
    // callback is fired. Aborting the NAK'd transfer to allow other transfers 
    // to happen on the one shared epx endpoint is needed to allow these devices 
    // to finish mounting and be in a usable state after enumeration.
    usbh_edpt_clear_in_on_nak(p_midi_host->dev_addr, p_midi_host->ep_in);
  }
  if (out_desc)
  {
    TU_ASSERT(usbh_edpt_open(rhport, dev_addr, out_desc));
  }
  p_midi_host->dev_addr = dev_addr;

  if (tuh_midi_mount_cb)
  {
    tuh_midi_mount_cb(dev_addr, p_midi_host->ep_in, p_midi_host->ep_out, p_midi_host->num_cables_rx, p_midi_host->num_cables_tx);
  }
  return true;
}

bool tuh_midi_configured(uint8_t dev_addr)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  return p_midi_host->configured;
}

bool midih_set_config(uint8_t dev_addr, uint8_t itf_num)
{
  (void) itf_num;
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  p_midi_host->configured = true;

  // TODO I don't think there are any special config things to do for MIDI

  return true;
}

//--------------------------------------------------------------------+
// Stream API
//--------------------------------------------------------------------+
static uint32_t write_flush(uint8_t dev_addr, midih_interface_t* midi)
{
  // No data to send
  if ( !tu_fifo_count(&midi->tx_ff) ) return 0;

  // skip if previous transfer not complete
  TU_VERIFY( usbh_edpt_claim(dev_addr, midi->ep_out) );

  uint16_t count = tu_fifo_read_n(&midi->tx_ff, midi->epout_buf, midi->ep_out_max);

  if (count)
  {
    TU_ASSERT( usbh_edpt_xfer(dev_addr, midi->ep_out, midi->epout_buf, count), 0 );
    return count;
  }else
  {
    // Release endpoint since we don't make any transfer
    usbh_edpt_release(dev_addr, midi->ep_out);
    return 0;
  }
}

bool tuh_midi_read_poll( uint8_t dev_addr )
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  // MIDI bulk endpoints are shared with the control endpoints. None can be busy before we start a transfer
  bool control_edpt_not_busy = !usbh_edpt_busy(dev_addr,0) && !usbh_edpt_busy(dev_addr,0x80);
  bool out_edpt_not_busy = true;
  bool result = false;
  if (p_midi_host->num_cables_tx > 0)
    out_edpt_not_busy = !usbh_edpt_busy(p_midi_host->dev_addr, p_midi_host->ep_out);
  if (control_edpt_not_busy && out_edpt_not_busy)
  {
    bool in_edpt_not_busy = !usbh_edpt_busy(dev_addr, p_midi_host->ep_in);
    if (in_edpt_not_busy)
    {
      TU_LOG2("Requesting poll IN endpoint %d\r\n", p_midi_host->ep_in);
      TU_ASSERT(usbh_edpt_xfer(p_midi_host->dev_addr, p_midi_host->ep_in, _midi_host->epin_buf, _midi_host->ep_in_max), 0);
      result = true;
    }
    else
    {
      // Maybe the IN endpoint is only busy because the RP2040 host hardware
      // is retrying a NAK'd IN transfer forever. Try aborting the NAK'd
      // transfer to allow other transfers to happen on the one shared
      // epx endpoint.
      usbh_edpt_clear_in_on_nak(p_midi_host->dev_addr, p_midi_host->ep_in);
    }
  }
  return result;
}

uint32_t tuh_midi_stream_write (uint8_t dev_addr, uint8_t cable_num, uint8_t const* buffer, uint32_t bufsize)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  TU_VERIFY(cable_num < p_midi_host->num_cables_tx);
  midi_stream_t *stream = &p_midi_host->stream_write;

  uint32_t i = 0;
  while ( (i < bufsize) && (tu_fifo_remaining(&p_midi_host->tx_ff) >= 4) )
  {
    uint8_t const data = buffer[i];
    i++;
    if (data >= MIDI_STATUS_SYSREAL_TIMING_CLOCK)
    {
      // real-time messages need to be sent right away
      midi_stream_t streamrt;
      streamrt.buffer[0] = MIDI_CIN_SYSEX_END_1BYTE;
      streamrt.buffer[1] = data;
      streamrt.index = 2;
      streamrt.total = 2;
      uint16_t const count = tu_fifo_write_n(&p_midi_host->tx_ff, streamrt.buffer, 4);
      // FIFO overflown, since we already check fifo remaining. It is probably race condition
      TU_ASSERT(count == 4, i);
    }
    else if ( stream->index == 0 )
    {
      //------------- New event packet -------------//

      uint8_t const msg = data >> 4;

      stream->index = 2;
      stream->buffer[1] = data;

      // Check to see if we're still in a SysEx transmit.
      if ( stream->buffer[0] == MIDI_CIN_SYSEX_START )
      {
        if ( data == MIDI_STATUS_SYSEX_END )
        {
          stream->buffer[0] = MIDI_CIN_SYSEX_END_1BYTE;
          stream->total = 2;
        }
        else
        {
          stream->total = 4;
        }
      }
      else if ( (msg >= 0x8 && msg <= 0xB) || msg == 0xE )
      {
        // Channel Voice Messages
        stream->buffer[0] = (cable_num << 4) | msg;
        stream->total = 4;
      }
      else if ( msg == 0xC || msg == 0xD)
      {
        // Channel Voice Messages, two-byte variants (Program Change and Channel Pressure)
        stream->buffer[0] = (cable_num << 4) | msg;
        stream->total = 3;
      }
      else if ( msg == 0xf )
      {
        // System message
        if ( data == MIDI_STATUS_SYSEX_START )
        {
          stream->buffer[0] = MIDI_CIN_SYSEX_START;
          stream->total = 4;
        }
        else if ( data == MIDI_STATUS_SYSCOM_TIME_CODE_QUARTER_FRAME || data == MIDI_STATUS_SYSCOM_SONG_SELECT )
        {
          stream->buffer[0] = MIDI_CIN_SYSCOM_2BYTE;
          stream->total = 3;
        }
        else if ( data == MIDI_STATUS_SYSCOM_SONG_POSITION_POINTER )
        {
          stream->buffer[0] = MIDI_CIN_SYSCOM_3BYTE;
          stream->total = 4;
        }
        else
        {
          stream->buffer[0] = MIDI_CIN_SYSEX_END_1BYTE;
          stream->total = 2;
        }
      }
      else
      {
        // Pack individual bytes if we don't support packing them into words.
        stream->buffer[0] = cable_num << 4 | 0xf;
        stream->buffer[2] = 0;
        stream->buffer[3] = 0;
        stream->index = 2;
        stream->total = 2;
      }
    }
    else
    {
      //------------- On-going (buffering) packet -------------//

      TU_ASSERT(stream->index < 4, i);
      stream->buffer[stream->index] = data;
      stream->index++;
      // See if this byte ends a SysEx.
      if ( stream->buffer[0] == MIDI_CIN_SYSEX_START && data == MIDI_STATUS_SYSEX_END )
      {
        stream->buffer[0] = MIDI_CIN_SYSEX_START + (stream->index - 1);
        stream->total = stream->index;
      }
    }

    // Send out packet
    if ( stream->index >= 2 && stream->index == stream->total )
    {
      // zeroes unused bytes
      for(uint8_t idx = stream->total; idx < 4; idx++) stream->buffer[idx] = 0;
      TU_LOG3_MEM(stream->buffer, 4, 2);

      uint16_t const count = tu_fifo_write_n(&p_midi_host->tx_ff, stream->buffer, 4);

      // complete current event packet, reset stream
      stream->index = 0;
      stream->total = 0;

      // FIFO overflown, since we already check fifo remaining. It is probably race condition
      TU_ASSERT(count == 4, i);
    }
  }
  return i;
}

uint32_t tuh_midi_stream_flush( uint8_t dev_addr )
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  bool control_edpt_not_busy = !usbh_edpt_busy(dev_addr,0) && !usbh_edpt_busy(dev_addr,0x80);
  bool in_edpt_not_busy = true;

  uint32_t bytes_flushed = 0;
  if (p_midi_host->num_cables_rx > 0)
    in_edpt_not_busy = !usbh_edpt_busy(dev_addr, p_midi_host->ep_in);
  if (control_edpt_not_busy && in_edpt_not_busy && !usbh_edpt_busy(p_midi_host->dev_addr, p_midi_host->ep_out))
  {
    bytes_flushed = write_flush(dev_addr, p_midi_host);
  }
  return bytes_flushed;
}
//--------------------------------------------------------------------+
// Helper
//--------------------------------------------------------------------+
uint8_t tuh_midih_get_num_tx_cables (uint8_t dev_addr)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  TU_VERIFY(p_midi_host->ep_out != 0); // returns 0 if fails
  return p_midi_host->num_cables_tx;
}

uint8_t tuh_midih_get_num_rx_cables (uint8_t dev_addr)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  TU_VERIFY(p_midi_host->ep_in != 0); // returns 0 if fails
  return p_midi_host->num_cables_rx;
}

uint32_t tuh_midi_stream_read (uint8_t dev_addr, uint8_t *p_cable_num, uint8_t *p_buffer, uint16_t bufsize)
{
  midih_interface_t *p_midi_host = get_midi_host(dev_addr);
  uint32_t bytes_buffered = 0;
  TU_ASSERT(p_cable_num);
  TU_ASSERT(p_buffer);
  TU_ASSERT(bufsize);
  uint8_t one_byte;
  if (!tu_fifo_peek(&p_midi_host->rx_ff, &one_byte))
  {
    return 0;
  }
  *p_cable_num = (one_byte >> 4) & 0xf;
  uint32_t nread = tu_fifo_read_n(&p_midi_host->rx_ff, p_midi_host->stream_read.buffer, 4);
  static uint16_t cable_sysex_in_progress; // bit i is set if received MIDI_STATUS_SYSEX_START but not MIDI_STATUS_SYSEX_END
  while (nread == 4 && bytes_buffered < bufsize)
  {
    *p_cable_num=(p_midi_host->stream_read.buffer[0] >> 4) & 0x0f;
    uint8_t bytes_to_add_to_stream = 0;
    if (*p_cable_num < p_midi_host->num_cables_rx)
    {
      // ignore the CIN field; too many devices out there encode this wrong
      uint8_t status = p_midi_host->stream_read.buffer[1];
      uint16_t cable_mask = 1 << *p_cable_num;
      if (status <= MIDI_MAX_DATA_VAL || status == MIDI_STATUS_SYSEX_START)
      {
        if (status == MIDI_STATUS_SYSEX_START)
        {
          cable_sysex_in_progress |= cable_mask;
        }
        // only add the packet if a sysex message is in progress
        if (cable_sysex_in_progress & cable_mask)
        {
          ++bytes_to_add_to_stream;
          uint8_t idx;
          for (idx = 2; idx < 4; idx++)
          {
            if (p_midi_host->stream_read.buffer[idx] <= MIDI_MAX_DATA_VAL)
            {
              ++bytes_to_add_to_stream;
            }
            else if (p_midi_host->stream_read.buffer[idx] == MIDI_STATUS_SYSEX_END)
            {
              ++bytes_to_add_to_stream;
              cable_sysex_in_progress &= ~cable_mask;
              idx = 4; // force the loop to exit; I hate break statements in loops
            }
          }
        }
      }
      else if (status < MIDI_STATUS_SYSEX_START)
      {
        // then it is a channel message either three bytes or two
        uint8_t fake_cin = (status & 0xf0) >> 4;
        switch (fake_cin)
        {
          case MIDI_CIN_NOTE_OFF:
          case MIDI_CIN_NOTE_ON:
          case MIDI_CIN_POLY_KEYPRESS:
          case MIDI_CIN_CONTROL_CHANGE:
          case MIDI_CIN_PITCH_BEND_CHANGE:
            bytes_to_add_to_stream = 3;
            break;
          case MIDI_CIN_PROGRAM_CHANGE:
          case MIDI_CIN_CHANNEL_PRESSURE:
            bytes_to_add_to_stream = 2;
            break;
          default:
            break; // Should not get this
        }
        cable_sysex_in_progress &= ~cable_mask;
      }
      else if (status < MIDI_STATUS_SYSREAL_TIMING_CLOCK)
      {
        switch (status)
        {
          case MIDI_STATUS_SYSCOM_TIME_CODE_QUARTER_FRAME:
          case MIDI_STATUS_SYSCOM_SONG_SELECT:
            bytes_to_add_to_stream = 2;
            break;
          case MIDI_STATUS_SYSCOM_SONG_POSITION_POINTER:
            bytes_to_add_to_stream = 3;
            break;
          case MIDI_STATUS_SYSCOM_TUNE_REQUEST:
          case MIDI_STATUS_SYSEX_END:
            bytes_to_add_to_stream = 1;
            break;
          default:
            break;
          cable_sysex_in_progress &= ~cable_mask;
        }
      }
      else
      {
        // Real-time message: can be inserted into a sysex message,
        // so do don't clear cable_sysex_in_progress bit
        bytes_to_add_to_stream = 1;
      }
    }
    uint8_t idx;
    for (idx = 1; idx <= bytes_to_add_to_stream; idx++)
    {
      *p_buffer++ = p_midi_host->stream_read.buffer[idx];
    }
    bytes_buffered += bytes_to_add_to_stream;
    nread = 0;
    if (tu_fifo_peek(&p_midi_host->rx_ff, &one_byte))
    {
      uint8_t new_cable = (one_byte >> 4) & 0xf;
      if (new_cable == *p_cable_num)
      {
        // still on the same cable. Continue reading the stream
        nread = tu_fifo_read_n(&p_midi_host->rx_ff, p_midi_host->stream_read.buffer, 4);
      }
    }
  }

  return bytes_buffered;
}
#endif

//} // extern C

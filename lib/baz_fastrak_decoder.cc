/* -*- c++ -*- */
/*
 * Copyright 2004 Free Software Foundation, Inc.
 * 
 * This file is part of GNU Radio
 * 
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * gr-baz by Balint Seeber (http://spench.net/contact)
 * Information, documentation & samples: http://wiki.spench.net/wiki/gr-baz
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <baz_fastrak_decoder.h>
#include <gnuradio/io_signature.h>

#include <stdio.h>
#include <boost/format.hpp>

/*
 * Create a new instance of baz_fastrak_decoder and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
baz_fastrak_decoder_sptr baz_make_fastrak_decoder(int sample_rate)
{
	return baz_fastrak_decoder_sptr(new baz_fastrak_decoder(sample_rate));
}

static const int MIN_IN = 2;	// mininum number of input streams
static const int MAX_IN = 2;	// maximum number of input streams
static const int MIN_OUT = 0;	// minimum number of output streams
static const int MAX_OUT = 2;	// maximum number of output streams

/*
 * The private constructor
 */
baz_fastrak_decoder::baz_fastrak_decoder(int sample_rate)
  : gr::sync_block("fastrak_decoder",
		gr::io_signature::make(MIN_IN, MAX_IN, sizeof(float)),
		gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof(float)))
  , d_sample_rate(sample_rate)
  , d_state(STATE_SEARCHING)
  , d_last_id(-1)
  , d_id(-1)
  , d_last_id_count(0)
{
	const int fastrak_rate = 300000;
	d_oversampling = sample_rate / fastrak_rate;
	
	fprintf(stderr, "[%s<%li>] sample rate: %d, oversampling: %d\n", name().c_str(), unique_id(), sample_rate, d_oversampling);
	
	d_type_length_map[PT_ID] = 32;
}

/*
 * Our virtual destructor.
 */
baz_fastrak_decoder::~baz_fastrak_decoder()
{
}

void baz_fastrak_decoder::set_sync_threshold(float threshold)
{
	fprintf(stderr, "[%s<%li>] sync threshold: %f (was: %f)\n", name().c_str(), unique_id(), threshold, d_sync_threshold);
	
	d_sync_threshold = threshold;
}

void baz_fastrak_decoder::enter_state(state_t state)
{
	d_state = state;
	d_bit_counter = 0;
	d_bit_buffer = 0;
}

static uint16_t crc16_compute(uint8_t data, uint16_t init)
{
	uint16_t crc;
	uint16_t t;

	t = ((init >> 8) ^ data) & 0xff;
	t ^= t >> 4;

	crc = (init << 8) ^ (t << 12) ^ (t << 5) ^ t;
	crc &= 0xffff;

	return crc;
}

static unsigned short crc16_update(unsigned short crc, unsigned char a)
{
	return crc16_compute(a, crc);
	
	const unsigned short poly = /*0x8408*/0x1021;	// CCITT

	crc ^= a;
	for (int i = 0; i < 8; ++i)
	{
		if (crc & 1)
			crc = (crc >> 1) ^ poly;
		else
			crc = (crc >> 1);
	}

	return crc;
}

unsigned int baz_fastrak_decoder::last_id_count(bool reset /*= false*/)
{
	unsigned int count = d_last_id_count;
	
	if (reset)
		d_last_id_count = 0;
	
	return count;
}

int baz_fastrak_decoder::work(int noutput_items, gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{
	const float *in = (const float*)input_items[0];
	const float *sync = (const float*)input_items[1];
	float *out = NULL;
	float *samp = NULL;
	if (output_items.size() > 0)
		out = (float*)output_items[0];
	if (output_items.size() > 1)
		samp = (float*)output_items[1];

	for (int i = 0; i < noutput_items; i++)
	{
		if (out)
			out[i] = in[i];
		if (samp)
			samp[i] = 0.0f;
		
		unsigned char bit = ((in[i] >= 0.0f) ? 0x1 : 0x0);
		
		switch (d_state)
		{
			case STATE_SEARCHING:
				if (sync[i] >= d_sync_threshold)
				{
					enter_state(STATE_SYNC);
					--i;
					d_sub_symbol_counter = 0;
					d_crc = 0x0000;	// NOT 0xFFFF (spec bug)
					d_total_bit_counter = 0;
					d_crc_buffer = 0;
					d_compute_crc = false;
					continue;
				}
				break;
			case STATE_SYNC:	// Peak aligned with sampling point on first bit
			case STATE_TYPE:
			case STATE_DECODE:
			case STATE_CRC:
				if (d_sub_symbol_counter > 0)
				{
					--d_sub_symbol_counter;
					continue;
				}
				
				if (samp)
					samp[i] = (bit ? 1.0f : -1.0f);
				
				d_sub_symbol_counter = d_oversampling - 1;
				d_bit_buffer <<= 1;
				d_bit_buffer |= bit;
				++d_bit_counter;
				++d_total_bit_counter;
				
				/*if ((d_total_bit_counter % 4) == 0)
				{
					unsigned int c = d_bit_buffer & 0xF;
					fprintf(stderr, "%X", c);// fflush(stderr);
				}*/
				
				//if (d_state != STATE_CRC)
				if (d_compute_crc)
				{
					d_crc_buffer <<= 1;
					d_crc_buffer |= bit;
					++d_crc_bit_counter;
					
					if ((d_crc_bit_counter % 8) == 0)
					{
						d_crc = crc16_update(d_crc, d_crc_buffer);
						d_crc_buffer = 0;
					}
				}
				
				if (d_state == STATE_SYNC)
				{
					if (d_bit_counter == 12)
					{
						if (d_bit_buffer == 0xAAC)
						{
							d_compute_crc = true;
							d_crc_bit_counter = 0;
							enter_state(STATE_TYPE);
						}
						else
							enter_state(STATE_SEARCHING);
					}
				}
				else if (d_state == STATE_TYPE)
				{
					if (d_bit_counter == 16)
					{
						d_current_packet_type = (packet_type_t)d_bit_buffer;
						TypeLengthMap::iterator it = d_type_length_map.find(d_current_packet_type);
						if (it == d_type_length_map.end())
							enter_state(STATE_SEARCHING);
						else
						{
							enter_state(STATE_DECODE);
							d_payload_bit_counter = it->second;
						}
					}
				}
				else if (d_state == STATE_DECODE)
				{
					/*switch (d_current_packet_type)
					{
						case //:
							//
							break;
					}*/
					
					if (d_bit_counter == d_payload_bit_counter)
					{
						switch (d_current_packet_type)
						{
							case PT_ID:
								/*if (d_bit_buffer != d_id)
								{
									fprintf(stderr, "%d (%08X)\n", d_bit_buffer, d_bit_buffer);// fflush(stderr);
								}*/
								d_id = d_bit_buffer;
								break;

							default:
								break;
						}
						
						enter_state(STATE_CRC);
					}
				}
				else if (d_state == STATE_CRC)
				{
					if (d_bit_counter == /*1*/16)
					{
						if ((d_crc_bit_counter % 8) != 0)
						{
							fprintf(stderr, "CRC bits left over: %d (%d bits in buffer)\n", (8 - d_crc_bit_counter), d_crc_bit_counter);
							//d_crc_buffer <<= (8 - d_crc_bit_counter);
							d_crc = crc16_update(d_crc, d_crc_buffer);
						}
					}
					
					if (d_bit_counter == 16)
					{
						if (d_crc == 0x00)
						{
							//fprintf(stderr, "+"); fflush(stderr);
							enter_state(STATE_DONE);
						}
						else
						{
							//fprintf(stderr, "_"); fflush(stderr);
							enter_state(STATE_SEARCHING);
						}
						
						//fprintf(stderr, "\n");
					}
				}
				break;
			case STATE_DONE:
				switch (d_current_packet_type)
				{
					case PT_ID:
						if (d_last_id != d_id)
						{
							fprintf(stderr, "%d (%08X)\n", d_id, d_id);
							
							d_last_id_count = 1;
						}
						else
						{
							fprintf(stderr, "."); fflush(stderr);
							++d_last_id_count;
						}
						
						d_last_id = d_id;
						d_last_id_string = str(boost::format("%d") % d_id);
						break;

					default:
						break;
				}
				--i;
				enter_state(STATE_SEARCHING);
				break;
			default:
				break;
		}
	}

	return noutput_items;
}

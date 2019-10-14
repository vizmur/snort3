//--------------------------------------------------------------------------
// Copyright (C) 2019-2019 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_hpack.cc author Katura Harvey <katharve@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_hpack.h"

#include "service_inspectors/http_inspect/http_field.h"
#include "service_inspectors/http_inspect/http_test_manager.h"

#include "http2_enum.h"
#include "http2_flow_data.h"

using namespace HttpCommon;
using namespace Http2Enums;

Http2HpackIntDecode Http2Hpack::decode_int7(7);
Http2HpackIntDecode Http2Hpack::decode_int6(6);
Http2HpackIntDecode Http2Hpack::decode_int5(5);
Http2HpackIntDecode Http2Hpack::decode_int4(4);
Http2HpackStringDecode Http2Hpack::decode_string;

bool Http2Hpack::write_decoded_headers(Http2FlowData* session_data, HttpCommon::SourceId source_id,
    const uint8_t* in_buffer, const uint32_t in_length,
    uint8_t* decoded_header_buffer, uint32_t decoded_header_length,
    uint32_t &bytes_written)
{
    bool ret = true;
    uint32_t length = in_length;
    bytes_written = 0;

    if (in_length > decoded_header_length)
    {
        length = MAX_OCTETS - session_data->http2_decoded_header_size[source_id];
        *session_data->infractions[source_id] += INF_DECODED_HEADER_BUFF_OUT_OF_SPACE;
        session_data->events[source_id]->create_event(EVENT_MISFORMATTED_HTTP2);
        ret = false;
    }

    memcpy((void*)decoded_header_buffer, (void*) in_buffer, length);
    bytes_written = length;
    return ret;
}

bool Http2Hpack::decode_string_literal(Http2FlowData* session_data, HttpCommon::SourceId source_id,
    const uint8_t* encoded_header_buffer, const uint32_t encoded_header_length,
    bool is_field_name, uint32_t &bytes_consumed,
    uint8_t* decoded_header_buffer, const uint32_t decoded_header_length,
    uint32_t &bytes_written)
{
    uint32_t decoded_bytes_written;
    uint32_t encoded_bytes_consumed;
    uint32_t encoded_header_offset = 0;
    bytes_written = 0;
    bytes_consumed = 0;

    if (is_field_name)
    {
        // skip over parsed pattern and zeroed index
        encoded_header_offset++;
        bytes_consumed++;
    }

    if (!decode_string.translate(encoded_header_buffer + encoded_header_offset,
        encoded_header_length, encoded_bytes_consumed, decoded_header_buffer,
        decoded_header_length, decoded_bytes_written, session_data->events[source_id],
        session_data->infractions[source_id]))
    {
        return false;
    }

    bytes_consumed += encoded_bytes_consumed;
    bytes_written += decoded_bytes_written;

    if (is_field_name)
    {
        if (!Http2Hpack::write_decoded_headers(session_data, source_id, (const uint8_t*)": ", 2,
                decoded_header_buffer + bytes_written, decoded_header_length -
                bytes_written, decoded_bytes_written))
            return false;
    }
    else
    {
        if (!Http2Hpack::write_decoded_headers(session_data, source_id, (const uint8_t*)"\r\n", 2,
                decoded_header_buffer + bytes_written, decoded_header_length -
                bytes_written, decoded_bytes_written))
            return false;
    }

    bytes_written += decoded_bytes_written;

    return true;
}

// FIXIT-H Will be incrementally updated to actually decode indexes. For now just copies encoded
// index directly to decoded_header_buffer
bool Http2Hpack::decode_index(Http2FlowData* session_data, HttpCommon::SourceId source_id,
    const uint8_t* encoded_header_buffer, const uint32_t encoded_header_length,
    const Http2HpackIntDecode &decode_int, uint32_t &bytes_consumed,
    uint8_t* decoded_header_buffer, const uint32_t decoded_header_length,
    uint32_t &bytes_written)
{
    uint64_t index;
    bytes_written = 0;
    bytes_consumed = 0;

    if (!decode_int.translate(encoded_header_buffer, encoded_header_length,
        bytes_consumed, index, session_data->events[source_id],
        session_data->infractions[source_id]))
    {
        return false;
    }

    if (index <= STATIC_TABLE_MAX_INDEX)
        decode_static_table_index();
    else
        decode_dynamic_table_index();

    if (!Http2Hpack::write_decoded_headers(session_data, source_id, encoded_header_buffer,
        bytes_consumed, decoded_header_buffer, decoded_header_length, bytes_written))
        return false;

    return true;
}

bool Http2Hpack::decode_literal_header_line(Http2FlowData* session_data,
    HttpCommon::SourceId source_id, const uint8_t* encoded_header_buffer,
    const uint32_t encoded_header_length, const uint8_t name_index_mask,
    const Http2HpackIntDecode &decode_int, uint32_t &bytes_consumed,
    uint8_t* decoded_header_buffer, const uint32_t decoded_header_length, uint32_t &bytes_written)
{
    bytes_written = 0;
    bytes_consumed = 0;
    uint32_t partial_bytes_consumed;
    uint32_t partial_bytes_written;
 
    // indexed field name
    if (encoded_header_buffer[0] & name_index_mask)
    {
        if (!Http2Hpack::decode_index(session_data, source_id, encoded_header_buffer,
                encoded_header_length, decode_int, partial_bytes_consumed,
                decoded_header_buffer, decoded_header_length, partial_bytes_written))
            return false;
    }
    // literal field name
    else
    {
        if (!Http2Hpack::decode_string_literal(session_data, source_id, encoded_header_buffer,
                encoded_header_length, true,
                partial_bytes_consumed, decoded_header_buffer, decoded_header_length,
                partial_bytes_written))
            return false;
    }

    bytes_consumed += partial_bytes_consumed;
    bytes_written += partial_bytes_written;

    // value is always literal
    if (!Http2Hpack::decode_string_literal(session_data, source_id, encoded_header_buffer +
            partial_bytes_consumed, encoded_header_length - partial_bytes_consumed,
            false, partial_bytes_consumed,
            decoded_header_buffer + partial_bytes_written, decoded_header_length -
            partial_bytes_written, partial_bytes_written))
        return false;

    bytes_consumed += partial_bytes_consumed;
    bytes_written += partial_bytes_written;

    return true;
}

// FIXIT-M Will be updated to actually update dynamic table size. For now just skips over
bool Http2Hpack::handle_dynamic_size_update(Http2FlowData* session_data,
    HttpCommon::SourceId source_id, const uint8_t* encoded_header_buffer,
    const uint32_t encoded_header_length, const Http2HpackIntDecode &decode_int,
    uint32_t &bytes_consumed, uint32_t &bytes_written)
{
    uint64_t decoded_int;
    uint32_t encoded_bytes_consumed;
    bytes_consumed = 0;
    bytes_written = 0;

    if (!decode_int.translate(encoded_header_buffer, encoded_header_length,
        encoded_bytes_consumed, decoded_int, session_data->events[source_id],
        session_data->infractions[source_id]))
    {
        return false;
    }
#ifdef REG_TEST
    //FIXIT-M remove when dynamic size updates are handled
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2))
    {
            fprintf(HttpTestManager::get_output_file(),
                "Skipping HPACK dynamic size update: %lu\n", decoded_int);
    }
#endif
    bytes_consumed += encoded_bytes_consumed;

    return true;
}

bool Http2Hpack::decode_header_line(Http2FlowData* session_data, HttpCommon::SourceId source_id,
    const uint8_t* encoded_header_buffer, const uint32_t encoded_header_length,
    uint32_t& bytes_consumed, uint8_t* decoded_header_buffer,
    const uint32_t decoded_header_length, uint32_t& bytes_written)
{
    const uint8_t index_mask = 0x80;
    const uint8_t literal_index_mask = 0x40;
    const uint8_t literal_index_name_index_mask = 0x3f;
    const uint8_t literal_no_index_mask = 0xf0;
    const uint8_t literal_never_index_pattern = 0x10;
    const uint8_t literal_no_index_name_index_mask = 0x0f;

    // indexed header representation
    if (encoded_header_buffer[0] & index_mask)
        return Http2Hpack::decode_index(session_data, source_id, encoded_header_buffer,
            encoded_header_length, decode_int7, bytes_consumed,
            decoded_header_buffer, decoded_header_length, bytes_written);

    // literal header representation to be added to dynamic table
    else if (encoded_header_buffer[0] & literal_index_mask)
        return Http2Hpack::decode_literal_header_line(session_data, source_id,
            encoded_header_buffer, encoded_header_length, literal_index_name_index_mask,
            decode_int6, bytes_consumed, decoded_header_buffer,
            decoded_header_length, bytes_written);

    // literal header field representation not to be added to dynamic table
    // Note that this includes two representation types from the RFC - literal without index and
    // literal never index. From a decoding standpoint these are identical.
    else if ((encoded_header_buffer[0] & literal_no_index_mask) == 0 or
            (encoded_header_buffer[0] & literal_no_index_mask) == literal_never_index_pattern)
        return Http2Hpack::decode_literal_header_line(session_data, source_id,
            encoded_header_buffer, encoded_header_length, literal_no_index_name_index_mask,
            decode_int4, bytes_consumed, decoded_header_buffer,
            decoded_header_length, bytes_written);
    else
        // FIXIT-M dynamic table size update not yet supported, just skip
        return handle_dynamic_size_update(session_data, source_id, encoded_header_buffer,
            encoded_header_length, decode_int5, bytes_consumed, bytes_written);
}

// FIXIT-H This will eventually be the decoded header buffer. For now only string literals are
// decoded
bool Http2Hpack::decode_headers(Http2FlowData* session_data, HttpCommon::SourceId source_id,
    const uint8_t* encoded_header_buffer, const uint32_t header_length)
{
    uint32_t total_bytes_consumed = 0;
    uint32_t line_bytes_consumed = 0;
    uint32_t line_bytes_written = 0;
    bool success = true;
    session_data->http2_decoded_header[source_id] = new uint8_t[MAX_OCTETS];
    session_data->http2_decoded_header_size[source_id] = 0;

    while (total_bytes_consumed < header_length)
    {
        if (!Http2Hpack::decode_header_line(session_data, source_id,
            encoded_header_buffer + total_bytes_consumed, header_length - total_bytes_consumed,
            line_bytes_consumed, session_data->http2_decoded_header[source_id] +
            session_data->http2_decoded_header_size[source_id], MAX_OCTETS -
            session_data->http2_decoded_header_size[source_id], line_bytes_written))
        {
            success = false;
            break;
        }
        total_bytes_consumed  += line_bytes_consumed;
        session_data->http2_decoded_header_size[source_id] += line_bytes_written;
    }

    if (!success)
    {
#ifdef REG_TEST
        if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2))
        {
            fprintf(HttpTestManager::get_output_file(), "Error decoding headers. ");
            if (session_data->http2_decoded_header_size[source_id] > 0)
                Field(session_data->http2_decoded_header_size[source_id],
                    session_data->http2_decoded_header[source_id]).print(
                    HttpTestManager::get_output_file(), "Partially Decoded Header");
        }
#endif
    return false;
    }

    // write the last CRLF to end the header
    if (!Http2Hpack::write_decoded_headers(session_data, source_id, (const uint8_t*)"\r\n", 2,
        session_data->http2_decoded_header[source_id] +
        session_data->http2_decoded_header_size[source_id], MAX_OCTETS -
        session_data->http2_decoded_header_size[source_id], line_bytes_written))
        return false;
    session_data->http2_decoded_header_size[source_id] += line_bytes_written;

#ifdef REG_TEST
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2))
    {
        Field(session_data->http2_decoded_header_size[source_id],
            session_data->http2_decoded_header[source_id]).
            print(HttpTestManager::get_output_file(), "Decoded Header");
    }
#endif

    return success;
}
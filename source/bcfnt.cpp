/*------------------------------------------------------------------------------
 * Copyright (c) 2019
 *     Michael Theall (mtheall)
 *     piepie62
 *
 * This file is part of tex3ds.
 *
 * tex3ds is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * tex3ds is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with tex3ds.  If not, see <http://www.gnu.org/licenses/>.
 *----------------------------------------------------------------------------*/
/** @file bcfnt.cpp
 *  @brief BCFNT definitions
 */

#include "magick_compat.h"

#include "bcfnt.h"
#include "freetype.h"
#include "future.h"
#include "quantum.h"
#include "swizzle.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

namespace
{
bool allowed (std::uint16_t code, const std::vector<std::uint16_t> &list, bool isBlacklist)
{
	return std::binary_search (std::begin (list), std::end (list), code) != isBlacklist;
}

void appendSheet (std::vector<std::uint8_t> &data, Magick::Image &sheet)
{
	swizzle (sheet, false);

	const unsigned w = sheet.columns ();
	const unsigned h = sheet.rows ();

	data.reserve (data.size () + w * h / 2);

	Pixels cache (sheet);
	for (unsigned y = 0; y < h; y += 8)
	{
		for (unsigned x = 0; x < w; x += 8)
		{
			PixelPacket p = cache.get (x, y, 8, 8);
			for (unsigned i = 0; i < 8 * 8; i += 2)
			{
				data.emplace_back ((quantum_to_bits<4> (quantumAlpha (p[i + 1])) << 4) |
				                   (quantum_to_bits<4> (quantumAlpha (p[i + 0])) << 0));
			}
		}
	}
}

Magick::Image unpackSheet (std::vector<std::uint8_t>::const_iterator &data,
    const unsigned WIDTH,
    const unsigned HEIGHT)
{
	Magick::Image ret (Magick::Geometry (WIDTH, HEIGHT), transparent ());

	Pixels cache (ret);
	for (unsigned y = 0; y < HEIGHT; y += 8)
	{
		for (unsigned x = 0; x < WIDTH; x += 8)
		{
			PixelPacket p = cache.get (x, y, 8, 8);
			for (unsigned i = 0; i < 8 * 8 / 2; ++i)
			{
				Magick::Color opacity;

				opacity.alphaQuantum (bits_to_quantum<4> ((*data) & 0xF));
				p[2 * i].opacity = quantumAlpha (opacity);

				opacity.alphaQuantum (bits_to_quantum<4> (((*data) >> 4) & 0xF));
				p[2 * i + 1].opacity = quantumAlpha (opacity);

				++data;
			}

			cache.sync ();
		}
	}

	swizzle (ret, true);

	return ret;
}

void coalesceCMAP (std::vector<bcfnt::CMAP> &cmaps)
{
	static constexpr auto MIN_CHARS          = 7;
	std::uint16_t codeBegin                  = 0xFFFF;
	std::uint16_t codeEnd                    = 0;
	std::unique_ptr<bcfnt::CMAPScan> scanMap = future::make_unique<bcfnt::CMAPScan> ();

	auto cmap = std::begin (cmaps);
	while (cmap != std::end (cmaps))
	{
		if (cmap->mappingMethod == bcfnt::CMAPData::CMAP_TYPE_DIRECT &&
		    cmap->codeEnd - cmap->codeBegin < MIN_CHARS - 1)
		{
			if (cmap->codeBegin < codeBegin)
				codeBegin = cmap->codeBegin;

			if (cmap->codeEnd > codeEnd)
				codeEnd = cmap->codeEnd;

			const auto &direct = dynamic_cast<bcfnt::CMAPDirect &> (*cmap->data);
			for (std::uint16_t i = cmap->codeBegin; i <= cmap->codeEnd; ++i)
				scanMap->entries.emplace (
				    i, static_cast<uint16_t> (i - cmap->codeBegin + direct.offset));

			cmap = cmaps.erase (cmap);
		}
		else
			++cmap;
	}

	if (scanMap->entries.empty ())
		return;

	cmaps.emplace_back (bcfnt::CMAP{codeBegin,
	    codeEnd,
	    static_cast<uint16_t> (bcfnt::CMAPData::CMAP_TYPE_SCAN),
	    0,
	    0,
	    std::move (scanMap)});
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, const char *str)
{
	const std::size_t len = std::strlen (str);

	o.reserve (o.size () + len);
	o.insert (std::end (o), str, str + len);

	return o;
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, std::uint8_t v)
{
	o.emplace_back (v);

	return o;
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, std::uint16_t v)
{
	o.reserve (o.size () + 2);
	o.emplace_back ((v >> 0) & 0xFF);
	o.emplace_back ((v >> 8) & 0xFF);

	return o;
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, std::uint32_t v)
{
	o.reserve (o.size () + 4);
	o.emplace_back ((v >> 0) & 0xFF);
	o.emplace_back ((v >> 8) & 0xFF);
	o.emplace_back ((v >> 16) & 0xFF);
	o.emplace_back ((v >> 24) & 0xFF);

	return o;
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, const bcfnt::CMAPScan &v)
{
	o << static_cast<uint16_t> (v.entries.size ());
	for (const auto &pair : v.entries)
		o << static_cast<uint16_t> (pair.first) << static_cast<uint16_t> (pair.second);

	return o;
}

std::vector<std::uint8_t> &operator<< (std::vector<std::uint8_t> &o, const bcfnt::CMAPTable &v)
{
	for (const auto &entry : v.table)
		o << static_cast<uint16_t> (entry);

	if (v.table.size () % 2)
		o << static_cast<uint16_t> (0);

	return o;
}

std::vector<std::uint8_t>::const_iterator &operator>> (std::vector<std::uint8_t>::const_iterator &i,
    std::uint32_t &v)
{
	v = *i++;
	v |= *i++ << 8;
	v |= *i++ << 16;
	v |= *i++ << 24;

	return i;
}

std::vector<std::uint8_t>::const_iterator &operator>> (std::vector<std::uint8_t>::const_iterator &i,
    std::uint16_t &v)
{
	v = *i++;
	v |= *i++ << 8;

	return i;
}

std::vector<std::uint8_t>::const_iterator &operator>> (std::vector<std::uint8_t>::const_iterator &i,
    std::uint8_t &v)
{
	v = *i++;

	return i;
}

std::vector<std::uint8_t>::const_iterator &operator>> (std::vector<std::uint8_t>::const_iterator &i,
    bcfnt::CharWidthInfo &v)
{
	v.left       = *i++;
	v.glyphWidth = *i++;
	v.charWidth  = *i++;

	return i;
}
}

namespace bcfnt
{
std::uint16_t CMAP::codePointFromIndex (std::uint16_t index) const
{
	switch (mappingMethod)
	{
	case bcfnt::CMAPData::CMAP_TYPE_DIRECT:
	{
		CMAPDirect &direct = dynamic_cast<CMAPDirect &> (*data);

		if (index < codeBegin + direct.offset)
			return 0xFFFF;

		if (index > codeEnd + direct.offset)
			return 0xFFFF;

		return codeBegin + index - direct.offset;
	}

	case bcfnt::CMAPData::CMAP_TYPE_TABLE:
	{
		CMAPTable &table = dynamic_cast<CMAPTable &> (*data);

		auto found = std::find (std::begin (table.table), std::end (table.table), index);
		if (found == std::end (table.table))
			return 0xFFFF;

		return codeBegin + std::distance (std::begin (table.table), found);
	}

	case bcfnt::CMAPData::CMAP_TYPE_SCAN:
	{
		CMAPScan &scan = dynamic_cast<CMAPScan &> (*data);

		auto match = [&](const std::pair<std::uint16_t, std::uint16_t> &pair) {
			return pair.second == index;
		};

		auto pair = std::find_if (std::begin (scan.entries), std::end (scan.entries), match);
		if (pair == std::end (scan.entries))
			return 0xFFFF;

		return pair->first;
	}

	default:
		std::abort ();
	}
}

void BCFNT::addFont (std::unique_ptr<freetype::Face> face,
    std::vector<std::uint16_t> &list,
    bool isBlacklist)
{
	int descent = std::numeric_limits<int>::max ();

	lineFeed = std::max (lineFeed, static_cast<std::uint8_t> ((*face)->size->metrics.height >> 6));
	height   = std::max (
        height, static_cast<std::uint8_t> (((*face)->bbox.yMax - (*face)->bbox.yMin) >> 6));
	width = std::max (
	    width, static_cast<std::uint8_t> (((*face)->bbox.xMax - (*face)->bbox.xMin) >> 6));
	maxWidth =
	    std::max (maxWidth, static_cast<std::uint8_t> ((*face)->size->metrics.max_advance >> 6));
	ascent  = std::max (ascent, static_cast<std::uint8_t> ((*face)->size->metrics.ascender >> 6));
	descent = std::min (descent, static_cast<int> ((*face)->size->metrics.descender) >> 6);

	// extract mappings from font face
	FT_UInt faceIndex;
	FT_ULong code = face->getFirstChar (faceIndex);
	while (faceIndex != 0)
	{
		// only supports 16-bit code points; also 0xFFFF is explicitly a non-character
		if (code >= std::numeric_limits<std::uint16_t>::max () || glyphs.count (code))
		{
			code = face->getNextChar (code, faceIndex);
			continue;
		}

		FT_Error error = face->loadGlyph (faceIndex, FT_LOAD_RENDER);
		if (error)
		{
			code = face->getNextChar (code, faceIndex);
			continue;
		}

		if (allowed (code, list, isBlacklist) && !glyphs.count (code))
		{
			ascent = std::max<int> (ascent, (*face)->glyph->bitmap_top);

			descent =
			    std::min<int> (descent, (*face)->glyph->bitmap_top - (*face)->glyph->bitmap.rows);

			maxWidth = std::max<std::uint8_t> (maxWidth, (*face)->glyph->bitmap.width);

			cellWidth   = maxWidth + 1;
			cellHeight  = ascent - descent;
			glyphWidth  = cellWidth + 1;
			glyphHeight = cellHeight + 1;

			glyphs.emplace (code, currentGlyphImage (face));
		}

		code = face->getNextChar (code, faceIndex);
	}

	if (glyphs.empty ())
		return;

	cellWidth      = maxWidth + 1;
	cellHeight     = ascent - descent;
	glyphWidth     = cellWidth + 1;
	glyphHeight    = cellHeight + 1;
	glyphsPerRow   = SHEET_WIDTH / glyphWidth;
	glyphsPerCol   = SHEET_HEIGHT / glyphHeight;
	glyphsPerSheet = glyphsPerRow * glyphsPerCol;

	// try to provide a replacement character
	if (glyphs.count (0xFFFD))
		altIndex = std::distance (std::begin (glyphs), glyphs.find (0xFFFD));
	else if (glyphs.count ('?'))
		altIndex = std::distance (std::begin (glyphs), glyphs.find ('?'));
	else if (glyphs.count (' '))
		altIndex = std::distance (std::begin (glyphs), glyphs.find (' '));
	else
		altIndex = 0;

	// collect character mappings
	refreshCMAPs ();

	numSheets = (glyphs.size () - 1) / glyphsPerSheet + 1;

	coalesceCMAP (cmaps);
}

BCFNT::BCFNT (const std::vector<std::uint8_t> &data)
{
	assert (data.size () >= 0x10);

	std::uint16_t in16;
	std::uint32_t in32;

	auto input = std::begin (data);

	input += 4;    // CFNT magic
	input >> in16; // BOM
	if (in16 != 0xFEFF)
	{
		std::fprintf (stderr, "No support for big-endian BCFNTs yet\n");
		return;
	}
	input += 2;                    // header size
	input += 4;                    // version
	input >> in32;                 // file size
	assert (in32 <= data.size ()); // Check the file size
	input += 4;                    // number of blocks

	input += 4; // FINF magic
	input += 4; // section size
	input += 1; // font type (still not sure about this thing)
	input >> lineFeed;
	input >> altIndex;
	input >> defaultWidth;
	input += 1; // encoding
	std::uint32_t tglpOffset;
	std::uint32_t cwdhOffset;
	std::uint32_t cmapOffset;
	input >> tglpOffset;
	input >> cwdhOffset;
	input >> cmapOffset;
	input >> height;
	input >> width;
	input >> ascent;
	input += 1; // padding

	// Do the CMAP stuff first
	while (cmapOffset != 0)
	{
		// Skip to right after CMAP magic, on section size
		input = std::begin (data) + cmapOffset - 4;

		input >> in32;          // CMAP size
		in32 -= 0x14;           // size without CMAP header
		assert (in32 % 4 == 0); // If it's not a multiple of four, something is very wrong
		bcfnt::CMAP cmap;
		input >> cmap.codeBegin; // Start codepoint
		input >> cmap.codeEnd;   // End codepoint
		input >> cmap.mappingMethod;
		input >> cmap.reserved;
		input >> cmapOffset;

		assert (cmap.codeEnd >= cmap.codeBegin);
		const std::uint16_t numCodes = cmap.codeEnd - cmap.codeBegin + 1;

		switch (cmap.mappingMethod)
		{
		case bcfnt::CMAPData::CMAP_TYPE_DIRECT:
			assert (in32 == 0x4);
			input >> in16;
			cmap.data = future::make_unique<bcfnt::CMAPDirect> (in16);
			break;

		case bcfnt::CMAPData::CMAP_TYPE_TABLE:
			assert (in32 == ((numCodes + 1u) & ~1u) * 2);

			cmap.data = future::make_unique<bcfnt::CMAPTable> ();
			for (std::uint16_t code = cmap.codeBegin; code <= cmap.codeEnd; ++code)
			{
				input >> in16;
				dynamic_cast<CMAPTable &> (*cmap.data).table.emplace_back (in16);
			}
			break;

		case bcfnt::CMAPData::CMAP_TYPE_SCAN:
		{
			input >> in16; // number of entries
			assert (in32 == (in16 + 1u) * 4);
			cmap.data = future::make_unique<bcfnt::CMAPScan> ();

			auto &scan = dynamic_cast<CMAPScan &> (*cmap.data);
			for (std::uint16_t entry = 0; entry < in16; ++entry)
			{
				std::uint16_t code;
				std::uint16_t index;

				input >> code >> index;
				scan.entries.emplace (code, index);
			}
			break;
		}

		default:
			std::abort ();
		}

		cmaps.emplace_back (std::move (cmap));
	}

	input = std::begin (data) + tglpOffset; // Fast forward to TGLP
	input >> cellWidth;
	input >> cellHeight;
	glyphWidth  = cellWidth + 1;
	glyphHeight = cellHeight + 1;
	input += 1; // baseline (same as ascent)
	input >> maxWidth;
	input >> SHEET_SIZE;
	input >> numSheets;
	input >> in16; // sheet format
	if (in16 != 0xB)
	{
		std::fprintf (stderr, "No formats except for 4-bit alpha currently supported");
		return;
	}
	input >> glyphsPerRow;
	input >> glyphsPerCol;
	glyphsPerSheet = glyphsPerRow * glyphsPerCol;
	input >> SHEET_WIDTH;
	input >> SHEET_HEIGHT;
	assert (SHEET_WIDTH * SHEET_HEIGHT / 2 == SHEET_SIZE);
	assert (SHEET_WIDTH / glyphWidth == glyphsPerRow);
	assert (SHEET_HEIGHT / glyphHeight == glyphsPerCol);
	input >> in32; // Sheet Offset
	input = std::begin (data) + in32;
	readGlyphImages (input, numSheets);

	while (cwdhOffset != 0)
	{
		// Skip to right after CWDH magic, on section size
		input = std::begin (data) + cwdhOffset - 4;

		input >> in32; // CWDH size
		in32 -= 0x10;  // size without CWDH header
		// assert(in32 % 3 == 0); // If it's not a multiple of three, something is very wrong
		std::uint16_t startIndex;
		input >> startIndex; // start index
		input >> in16;       // end index
		// assert(in32 == static_cast<std::uint16_t>(in16 - startIndex) * 3);
		input >> cwdhOffset;
		assert (in16 <= glyphs.size ());
		for (std::uint16_t glyph = startIndex; glyph < in16; ++glyph)
			input >> glyphs[codepoint (glyph)].info;
	}
}

bool BCFNT::serialize (const std::string &path)
{
	if (glyphs.empty ())
	{
		std::fprintf (stderr, "Empty font\n");
		return false;
	}
	std::vector<Magick::Image> sheetImages = sheetify ();
	std::vector<std::uint8_t> output;

	std::uint32_t fileSize = 0;
	fileSize += 0x14; // CFNT header

	const std::uint32_t finfOffset = fileSize;
	fileSize += 0x20; // FINF header

	const std::uint32_t tglpOffset = fileSize;
	fileSize += 0x20; // TGLP header

	constexpr std::uint32_t ALIGN   = 0x80;
	constexpr std::uint32_t MASK    = ALIGN - 1;
	const std::uint32_t sheetOffset = (fileSize + MASK) & ~MASK;
	fileSize                        = sheetOffset + sheetImages.size () * SHEET_SIZE;

	// CWDH headers + data
	const std::uint32_t cwdhOffset = fileSize;
	fileSize += 0x10;                          // CWDH header
	fileSize += (3 * glyphs.size () + 3) & ~3; // CWDH data

	// CMAP headers + data
	std::uint32_t cmapOffset = fileSize;
	for (const auto &cmap : cmaps)
	{
		fileSize += 0x14; // CMAP header

		switch (cmap.mappingMethod)
		{
		case CMAPData::CMAP_TYPE_DIRECT:
			fileSize += 0x4;
			break;

		case CMAPData::CMAP_TYPE_TABLE:
			fileSize += dynamic_cast<const CMAPTable &> (*cmap.data).table.size () * 2 +
			            (dynamic_cast<const CMAPTable &> (*cmap.data).table.size () % 2) * 2;
			break;

		case CMAPData::CMAP_TYPE_SCAN:
			fileSize += 4 + dynamic_cast<const CMAPScan &> (*cmap.data).entries.size () * 4;
			break;

		default:
			std::abort ();
		}
	}

	// FINF, TGLP, CWDH, CMAPs
	std::uint32_t numBlocks = 3 + cmaps.size ();

	// CFNT header
	output << "CFNT"                                  // magic
	       << static_cast<std::uint16_t> (0xFEFF)     // byte-order-mark
	       << static_cast<std::uint16_t> (0x14)       // header size
	       << static_cast<std::uint8_t> (0x0)         // version (?)
	       << static_cast<std::uint8_t> (0x0)         // version (?)
	       << static_cast<std::uint8_t> (0x0)         // version (?)
	       << static_cast<std::uint8_t> (0x3)         // version
	       << static_cast<std::uint32_t> (fileSize)   // file size
	       << static_cast<std::uint32_t> (numBlocks); // number of blocks

	// FINF header
	assert (output.size () == finfOffset);
	output << "FINF"                                              // magic
	       << static_cast<std::uint32_t> (0x20)                   // section size
	       << static_cast<std::uint8_t> (0x1)                     // font type
	       << static_cast<std::uint8_t> (lineFeed)                // line feed
	       << static_cast<std::uint16_t> (altIndex)               // alternate char index
	       << static_cast<std::uint8_t> (defaultWidth.left)       // default width (left)
	       << static_cast<std::uint8_t> (defaultWidth.glyphWidth) // default width (glyph width)
	       << static_cast<std::uint8_t> (defaultWidth.charWidth)  // default width (char width)
	       << static_cast<std::uint8_t> (0x1)                     // encoding
	       << static_cast<std::uint32_t> (tglpOffset + 8)         // TGLP offset
	       << static_cast<std::uint32_t> (cwdhOffset + 8)         // CWDH offset
	       << static_cast<std::uint32_t> (cmapOffset + 8)         // CMAP offset
	       << static_cast<std::uint8_t> (height)                  // font height
	       << static_cast<std::uint8_t> (width)                   // font width
	       << static_cast<std::uint8_t> (ascent)                  // font ascent
	       << static_cast<std::uint8_t> (0x0);                    // padding

	// TGLP header
	assert (output.size () == tglpOffset);
	output << "TGLP"                                    // magic
	       << static_cast<std::uint32_t> (0x20)         // section size
	       << static_cast<std::uint8_t> (cellWidth)     // cell width
	       << static_cast<std::uint8_t> (cellHeight)    // cell height
	       << static_cast<std::uint8_t> (ascent)        // cell baseline
	       << static_cast<std::uint8_t> (maxWidth)      // max character width
	       << static_cast<std::uint32_t> (SHEET_SIZE)   // sheet data size
	       << static_cast<std::uint16_t> (numSheets)    // number of sheets
	       << static_cast<std::uint16_t> (0xB)          // 4-bit alpha format
	       << static_cast<std::uint16_t> (glyphsPerRow) // num columns
	       << static_cast<std::uint16_t> (glyphsPerCol) // num rows
	       << static_cast<std::uint16_t> (SHEET_WIDTH)  // sheet width
	       << static_cast<std::uint16_t> (SHEET_HEIGHT) // sheet height
	       << static_cast<std::uint32_t> (sheetOffset); // sheet data offset

	assert (output.size () <= sheetOffset);
	output.resize (sheetOffset);
	assert (output.size () == sheetOffset);
	for (auto sheet : sheetImages)
	{
		appendSheet (output, sheet);
	}

	// CWDH header + data
	assert (output.size () == cwdhOffset);

	output << "CWDH"                                                              // magic
	       << static_cast<std::uint32_t> (0x10 + ((3 * glyphs.size () + 3) & ~3)) // section size
	       << static_cast<std::uint16_t> (0)                                      // start index
	       << static_cast<std::uint16_t> (glyphs.size ())                         // end index
	       << static_cast<std::uint32_t> (0); // next CWDH offset

	for (const auto &info : glyphs)
	{
		output << static_cast<std::uint8_t> (info.second.info.left)
		       << static_cast<std::uint8_t> (info.second.info.glyphWidth)
		       << static_cast<std::uint8_t> (info.second.info.charWidth);
	}

	while (output.size () & 0x3)
		output << static_cast<std::uint8_t> (0);

	for (const auto &cmap : cmaps)
	{
		assert (output.size () == cmapOffset);

		std::uint32_t size;
		switch (cmap.mappingMethod)
		{
		case bcfnt::CMAPData::CMAP_TYPE_DIRECT:
			size = 0x14 + 0x4;
			break;

		case bcfnt::CMAPData::CMAP_TYPE_TABLE:
			size = 0x14 + dynamic_cast<bcfnt::CMAPTable &> (*cmap.data).table.size () * 2 +
			       (dynamic_cast<bcfnt::CMAPTable &> (*cmap.data).table.size () % 2) * 2;
			break;

		case bcfnt::CMAPData::CMAP_TYPE_SCAN:
			size = 0x14 + 4 + dynamic_cast<bcfnt::CMAPScan &> (*cmap.data).entries.size () * 4;
			break;

		default:
			std::abort ();
		}

		output << "CMAP"                                          // magic
		       << static_cast<std::uint32_t> (size)               // section size
		       << static_cast<std::uint16_t> (cmap.codeBegin)     // code begin
		       << static_cast<std::uint16_t> (cmap.codeEnd)       // code end
		       << static_cast<std::uint16_t> (cmap.mappingMethod) // mapping method
		       << static_cast<std::uint16_t> (0x0);               // padding

		// next CMAP offset
		if (&cmap == &cmaps.back ())
			output << static_cast<std::uint32_t> (0);
		else
			output << static_cast<std::uint32_t> (cmapOffset + size + 8);

		switch (cmap.mappingMethod)
		{
		case CMAPData::CMAP_TYPE_DIRECT:
		{
			const auto &direct = dynamic_cast<const CMAPDirect &> (*cmap.data);
			output << static_cast<std::uint16_t> (direct.offset);
			output << static_cast<std::uint16_t> (0); // alignment
			break;
		}

		case CMAPData::CMAP_TYPE_TABLE:
		{
			const auto &table = dynamic_cast<const CMAPTable &> (*cmap.data);
			output << table;
			break;
		}

		case CMAPData::CMAP_TYPE_SCAN:
		{
			const auto &scan = dynamic_cast<const CMAPScan &> (*cmap.data);
			output << scan;
			output << static_cast<std::uint16_t> (0); // alignment
			break;
		}

		default:
			std::abort ();
		}

		cmapOffset += size;
	}

	assert (output.size () == fileSize);

	FILE *fp = std::fopen (path.c_str (), "wb");
	if (!fp)
		return false;

	std::size_t offset = 0;
	while (offset < output.size ())
	{
		std::size_t rc = std::fwrite (&output[offset], 1, output.size () - offset, fp);
		if (rc != output.size () - offset)
		{
			if (rc == 0 || std::ferror (fp))
			{
				if (std::ferror (fp))
					std::fprintf (stderr, "fwrite: %s\n", std::strerror (errno));
				else
					std::fprintf (stderr, "fwrite: Unknown write failure\n");

				std::fclose (fp);
				return false;
			}
		}

		offset += rc;
	}

	if (std::fclose (fp) != 0)
	{
		std::fprintf (stderr, "fclose: %s\n", std::strerror (errno));
		return false;
	}

	std::printf ("Generated font with %zu glyphs\n", glyphs.size ());
	return true;
}

std::vector<Magick::Image> BCFNT::sheetify ()
{
	std::map<std::uint16_t, bcfnt::Glyph>::iterator currentGlyph = std::begin (glyphs);

	std::vector<Magick::Image> ret;

	for (unsigned sheet = 0; sheet * glyphsPerSheet < glyphs.size (); ++sheet)
	{
		Magick::Image sheetData (Magick::Geometry (SHEET_WIDTH, SHEET_HEIGHT), transparent ());

		Pixels cache (sheetData);
		for (unsigned y = 0; y < glyphsPerCol; ++y)
		{
			for (unsigned x = 0; x < glyphsPerRow; ++x)
			{
				const unsigned glyphIndex =
				    sheet * glyphsPerRow * glyphsPerCol + y * glyphsPerRow + x;
				if (glyphIndex >= glyphs.size ())
				{
					ret.emplace_back (std::move (sheetData));
					return ret;
				}

				sheetData.composite (currentGlyph->second.img,
				    x * glyphWidth + 1,
				    y * glyphHeight + 1 + ascent - currentGlyph->second.ascent,
				    Magick::OverCompositeOp);

				++currentGlyph;
			}
		}

		ret.emplace_back (std::move (sheetData));
	}

	return ret;
}

Glyph BCFNT::currentGlyphImage (std::unique_ptr<freetype::Face> &face) const
{
	Glyph ret{Magick::Image (Magick::Geometry (glyphWidth, glyphHeight), transparent ()),
	    CharWidthInfo{static_cast<std::int8_t> ((*face)->glyph->metrics.horiBearingX >> 6),
	        static_cast<std::uint8_t> ((*face)->glyph->metrics.width >> 6),
	        static_cast<std::uint8_t> ((*face)->glyph->metrics.horiAdvance >> 6)},
	    ascent};

	const unsigned width  = (*face)->glyph->bitmap.width;
	const unsigned height = (*face)->glyph->bitmap.rows;
	const int yOffset     = ascent - (*face)->glyph->bitmap_top;

	Pixels cache (ret.img);
	PixelPacket p = cache.get (1, 1, cellWidth, cellHeight);
	for (unsigned y = 0; y < height; ++y)
	{
		for (unsigned x = 0; x < width; ++x)
		{
			const int py = y + yOffset;

			if (x >= cellWidth || py < 0 || py >= cellHeight)
				continue;

			const std::uint8_t v = (*face)->glyph->bitmap.buffer[y * width + x];

			Magick::Color c;
			quantumRed (c, bits_to_quantum<8> (0));
			quantumGreen (c, bits_to_quantum<8> (0));
			quantumBlue (c, bits_to_quantum<8> (0));
			quantumAlpha (c, bits_to_quantum<8> (v));

			p[py * cellWidth + x] = c;
		}
	}
	cache.sync ();

	return ret;
}

std::uint16_t BCFNT::codepoint (std::uint16_t index) const
{
	for (auto &cmap : cmaps)
	{
		std::uint16_t code = cmap.codePointFromIndex (index);
		if (code != 0xFFFF)
			return code;
	}

	return 0xFFFF;
}

void BCFNT::readGlyphImages (std::vector<std::uint8_t>::const_iterator &it, int numSheets)
{
	for (int sheet = 0; sheet < numSheets; ++sheet)
	{
		Magick::Image sheetData = unpackSheet (it, SHEET_WIDTH, SHEET_HEIGHT);

		Pixels cache (sheetData);
		for (unsigned y = 0; y < glyphsPerCol; ++y)
		{
			for (unsigned x = 0; x < glyphsPerRow; ++x)
			{
				PixelPacket glyphData =
				    cache.get (x * glyphWidth + 1, y * glyphHeight + 1, cellWidth, cellHeight);

				Magick::Image glyph (Magick::Geometry (glyphWidth, glyphHeight), transparent ());

				Pixels glyphPixels (glyph);
				PixelPacket outData = glyphPixels.get (0, 0, cellWidth, cellHeight);
				for (unsigned pixel = 0; pixel < cellWidth * cellHeight; ++pixel)
					outData[pixel] = glyphData[pixel];

				glyphPixels.sync ();

				const std::uint16_t code =
				    codepoint (sheet * glyphsPerSheet + y * glyphsPerRow + x);

				if (code != 0xFFFF)
					glyphs.emplace (code, Glyph{glyph, bcfnt::CharWidthInfo{0, 0, 0}, ascent});
			}
		}
	}
}

void BCFNT::refreshCMAPs ()
{
	cmaps.clear ();

	std::uint16_t index = 0;
	for (const auto &pair : glyphs)
	{
		const auto &code = pair.first;

		if (cmaps.empty () || cmaps.back ().codeEnd != code - 1)
		{
			cmaps.emplace_back ();
			auto &cmap = cmaps.back ();

			cmap.codeBegin = cmap.codeEnd = code;
			cmap.data                     = future::make_unique<CMAPDirect> (index);
			cmap.mappingMethod            = cmap.data->type ();
		}
		else
			cmaps.back ().codeEnd = code;

		++index;
	}
}

void BCFNT::addFont (BCFNT &other, std::vector<std::uint16_t> &list, bool isBlacklist)
{
	std::uint8_t newAscent = std::max (other.ascent, ascent);
	std::uint8_t newCellHeight =
	    newAscent + std::max (other.cellHeight - other.ascent, cellHeight - ascent);
	std::uint8_t newCellWidth = std::max (other.cellWidth, cellWidth);

	for (const auto &pair : other.glyphs)
	{
		const auto &code = pair.first;

		if (code != 0xFFFF && !glyphs.count (code) && allowed (code, list, isBlacklist))
			glyphs.emplace (pair);
	}

	refreshCMAPs ();

	ascent         = newAscent;
	cellHeight     = newCellHeight;
	cellWidth      = newCellWidth;
	glyphHeight    = cellHeight + 1;
	glyphWidth     = cellWidth + 1;
	glyphsPerRow   = SHEET_WIDTH / glyphWidth;
	glyphsPerCol   = SHEET_HEIGHT / glyphHeight;
	glyphsPerSheet = glyphsPerRow * glyphsPerCol;
	lineFeed       = std::max (lineFeed, other.lineFeed);
	height         = std::max (height, other.height);
	width          = std::max (width, other.width);
	maxWidth       = cellWidth;
	numSheets      = glyphs.size () / glyphsPerSheet + (glyphs.size () % glyphsPerSheet ? 1 : 0);
}
}

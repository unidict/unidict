# BGL File Format Specification

> Babylon BGL Dictionary File Format - Complete Technical Reference
>
> Based on analysis of GoldenDict and pyglossary implementations

---

## Table of Contents

1. [Overview](#1-overview)
2. [File Structure](#2-file-structure)
3. [File Header](#3-file-header)
4. [Block Structure](#4-block-structure)
5. [Metadata Blocks (Type 3)](#5-metadata-blocks-type-3)
6. [Entry Blocks (Type 1, 7, 10)](#6-entry-blocks-type-1-7-10)
7. [Entry Block Variant 3 (Type 11)](#7-entry-block-variant-3-type-11)
8. [Resource Blocks (Type 2)](#8-resource-blocks-type-2)
9. [Charset Tags](#9-charset-tags)
10. [Parsing Workflow](#10-parsing-workflow)
11. [Implementation Notes](#11-implementation-notes)
12. [References](#12-references)

---

## 1. Overview

The Babylon BGL (Babylon Glossary) file format is a proprietary dictionary format used by Babylon dictionaries. BGL files consist of a small uncompressed header followed by gzip-compressed data containing dictionary entries, metadata, and embedded resources.

### Key Characteristics

- **Compression**: Gzip-compressed data stream
- **Encoding**: Various encodings (Windows-1252, UTF-8, GB18030, etc.)
- **Structure**: Sequential block-based format
- **Access**: Sequential only (no random access without external index)

### Design Limitations

- Metadata blocks can appear anywhere in the file
- Two full passes required for complete parsing (metadata + entries)
- No built-in indexing mechanism

---

## 2. File Structure

```
┌──────────────────────────────────────────────────────────┐
│                      BGL File Layout                     │
├──────────────────────────────────────────────────────────┤
│  ┌────────────────┐                                      │
│  │  File Header   │  ← Uncompressed (6 bytes)            │
│  │    (6 bytes)   │                                      │
│  ├────────────────┤                                      │
│  │  +0: Signature │ 0x12340001 or 0x12340002 (big-endian)│
│  │  +4: Gzip Off  │ Offset to gzip data (big-endian)     │ 
│  └────────────────┘                                      │
│           │                                              │
│           │ seek to gzip_offset                          │
│           ▼                                              │
│  ┌────────────────────────────────────────────────────┐  │
│  │           Gzip Compressed Data Stream              │  │
│  │       (starts at gzip_offset)                      │  │
│  ├────────────────────────────────────────────────────┤  │
│  │                                                    │  │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐   │  │
│  │  │Block│→│Block│→│Block│→│Block│→│ ... │→│Block│   │  │
│  │  │     │ │     │ │     │ │     │ │     │ │ EOF │   │  │
│  │  └─────┘ └─────┘ └─────┘ └─────┘ └─────┘ └─────┘   │  │
│  │                                                    │  │
│  │  Each block contains:                              │  │
│  │  - Block header (1 byte: type + length encoding)   │  │
│  │  - Block data (variable length)                    │  │
│  │                                                    │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘

Block Types:
  - Type 0:  Extended (requires subtype byte)
  - Type 1:  Dictionary Entry (standard)
  - Type 2:  Resource (images, HTML, etc.)
  - Type 3:  Metadata (Extended with 2-byte code)
  - Type 4:  End of File
  - Type 6:  Entries Start Marker
  - Type 7:  Entry Variant 1
  - Type 10: Entry Variant 2
  - Type 11: Entry Variant 3 (different format)
  - Type 13: Entry Variant 4
```

---

## 3. File Header

**Location**: Bytes `+0` to `+5` (uncompressed)

**Structure**:

```
Offset  Size  Field          Description
─────────────────────────────────────────────────────────────
+0      4     Signature      File signature
                          • 0x12340001 (big-endian)
                          • 0x12340002 (big-endian)

+4      2     Gzip Offset   Offset to gzip data stream
                          • Big-endian 16-bit integer
                          • Always ≥ 6
                          • Points to gzip stream start
```

### 3.1 Signature Verification

```c
uint8_t header[6];
fread(header, 1, 6, file);

// Verify signature (big-endian)
uint32_t signature = (header[0] << 24) | (header[1] << 16) |
                     (header[2] << 8)  | header[3];

if (signature != 0x12340001 && signature != 0x12340002) {
    // Invalid BGL file
}

// Read gzip offset (big-endian)
uint16_t gzip_offset = (header[4] << 8) | header[5];
```

### 3.2 Opening the Gzip Stream

```c
// Method 1: Direct fileno (Linux)
FILE *fp = fopen(filename, "rb");
fseek(fp, gzip_offset, SEEK_SET);
gzFile gzf = gzdopen(fileno(fp), "rb");

// Method 2: Using dup (macOS/cross-platform, RECOMMENDED)
int fd = dup(fileno(fp));
lseek(fd, gzip_offset, SEEK_SET);
gzFile gzf = gzdopen(fd, "rb");
fclose(fp);  // Close original FILE*, keep only gzf
```

**Note**: On macOS, the `dup()` approach is required because the file position of a `FILE*` and its underlying file descriptor can become out of sync.

---

## 4. Block Structure

Each block consists of a **header** (1 byte) followed by **data** (variable length):

```
┌────────────────────────────────────────────────────────┐
│                       Block Format                     │
├────────────────────────────────────────────────────────┤
│                                                        │
│  ┌────────────────┐                                    │
│  │  Header (1B)   │  First byte encodes type + length  │
│  ├────────────────┤                                    │
│  │  ┌──────────┐  │                                    │
│  │  │ Bits 7-4 │  │  Length Encoding (4 bits)          │
│  │  │ Bits 3-0 │  │  Block Type (4 bits)               │
│  │  └──────────┘  │                                    │
│  └────────────────┘                                    │
│           │                                            │
│           ├─ If length encoding < 4: read extra bytes  │
│           │                                            │
│           ▼                                            │
│  ┌─────────────────────────────────────────────┐       │
│  │              Block Data (variable)          │       │
│  ├─────────────────────────────────────────────┤       │
│  │                                             │       │
│  │  Content depends on block type:             │       │
│  │  • Type 0:  Subtype (1B) + Data             │       │
│  │  • Type 1:  Entry (see section 6)           │       │
│  │  • Type 2:  Resource                        │       │
│  │  • Type 3:  Metadata (see section 5)        │       │
│  │  • Type 4:  EOF (no data)                   │       │
│  │  • Type 7/10/11: Entry variants             │       │
│  │                                             │       │
│  └─────────────────────────────────────────────┘       │
│                                                        │
└────────────────────────────────────────────────────────┘
```

### 4.1 Block Header Parsing

```c
// Read first byte
uint8_t first;
gzread(gzf, &first, 1);

uint8_t type = first & 0x0F;       // Lower 4 bits: block type
uint8_t length_code = first >> 4;  // Upper 4 bits: length encoding

// Calculate actual data length
size_t data_len;

if (length_code < 4) {
    // Read extra (length_code + 1) bytes for length
    uint8_t extra_len[4];
    gzread(gzf, extra_len, length_code + 1);

    // Combine as big-endian
    if (length_code == 0)
        data_len = extra_len[0];
    else if (length_code == 1)
        data_len = (extra_len[0] << 8) | extra_len[1];
    else if (length_code == 2)
        data_len = (extra_len[0] << 16) | (extra_len[1] << 8) | extra_len[2];
    else if (length_code == 3)
        data_len = (extra_len[0] << 24) | (extra_len[1] << 16) |
                   (extra_len[2] << 8) | extra_len[3];
} else {
    // Length is directly encoded
    data_len = length_code - 4;
}

// Read block data
uint8_t *data = malloc(data_len + 1);
gzread(gzf, data, data_len);
data[data_len] = '\0';
```

### 4.2 Block Length Encoding Rules

The block length encoding follows these rules:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Block Length Encoding Rules                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  First Byte Structure:                                             │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Bit 7  6  5  4  │  Bit 3  2  1  0                           │  │
│  ├─────────────────┼──────────────────────────────────────────────┤  │
│  │  Length Encoding │         Block Type                         │  │
│  │      (4 bits)    │           (4 bits)                         │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  Length Encoding Rules:                                            │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  length_code = (first_byte >> 4)                              │  │
│  │                                                               │  │
│  │  If length_code < 4:                                         │  │
│  │    data_len = bgl_readnum(length_code + 1)                   │  │
│  │    → Read extra (length_code + 1) bytes as big-endian value │  │
│  │                                                               │  │
│  │  If length_code >= 4:                                       │  │
│  │    data_len = length_code - 4                               │  │
│  │    → Length is directly encoded                             │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  bgl_readnum(n) reads n bytes and combines them as big-endian:    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  for (i = 0; i < n; i++) {                                  │  │
│  │    val = (val << 8) | buf[i];                               │  │
│  │  }                                                          │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

Examples:

┌──────────────┬──────────┬──────────────┬───────────────────────────┐
│ first_byte   │   type   │ length_code  │      data_len             │
├──────────────┼──────────┼──────────────┼───────────────────────────┤
│ 0x04         │ 4        │ 0            │ bgl_readnum(1) = value    │
│ 0x12         │ 2        │ 1            │ bgl_readnum(2) = big-endian│
│ 0x35         │ 5        │ 3            │ bgl_readnum(4) = big-endian│
│ 0x41         │ 1        │ 4            │ 4 - 4 = 0                │
│ 0x42         │ 2        │ 4            │ 4 - 4 = 0                │
│ 0x4A         │ 10       │ 4            │ 10 - 4 = 6               │
│ 0x4E         │ 14       │ 4            │ 14 - 4 = 10              │
└──────────────┴──────────┴──────────────┴───────────────────────────┘

Complete Parsing Algorithm (from GoldenDict):

    uint32_t first = bgl_readnum(1);
    uint8_t type = first & 0x0F;
    if (type == 4) return false;  // EOF

    uint8_t length_code = first >> 4;
    uint32_t data_len;

    if (length_code < 4)
        data_len = bgl_readnum(length_code + 1);  // Read extra bytes
    else
        data_len = length_code - 4;                 // Direct encoding
```

### 4.3 Block Type Reference

| Type | Name | Description | Data Structure |
|------|------|-------------|----------------|
| 0 | Extended | Extended type, requires subtype byte | `1B subtype + data` |
| 1 | Entry | Standard dictionary entry | See Section 6 |
| 2 | Resource | Embedded resource (image/HTML) | `1B name_len + name + data` |
| 3 | Extended² | Metadata block | `2B code + data` |
| 4 | EOF | End of file marker | No data |
| 6 | Entries Start | Entries region begins | No data |
| 7 | Entry Alt1 | Entry variant 1 | Similar to Type 1 |
| 10 | Entry Alt2 | Entry variant 2 | Similar to Type 1 |
| 11 | Entry Alt3 | Entry variant 3 | Different format (Section 7) |
| 13 | Entry Alt4 | Entry variant 4 | Similar to Type 1 |

---

## 5. Metadata Blocks (Type 3)

Type 3 blocks contain dictionary metadata. The block starts with a 2-byte code (big-endian) followed by type-specific data.

```
┌───────────────────────────────────────────────────────────┐
│                   Type 3 Metadata Block                   │
├───────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌────────────────────────────────────┐  │
│  │  Code (2B)  │  │            Data (variable)         │  │
│  │ (big-endian)│  │                                    │  │
│  ├─────────────┤  ├────────────────────────────────────┤  │
│  │             │  │                                    │  │
│  │  High Byte  │  │  Content depends on Code value     │  │
│  │  Low Byte   │  │                                    │  │
│  │             │  │  Code=0x01:  Title (string)        │  │
│  │             │  │  Code=0x02:  Author (string)       │  │
│  │             │  │  Code=0x07:  Source Lang (4B)      │  │
│  │             │  │  Code=0x08:  Target Lang (4B)      │  │
│  │             │  │  Code=0x11:  Flags (4B)            │  │
│  │             │  │  Code=0x1A:  Source Charset (1B)   │  │
│  │             │  │  Code=0x1B:  Target Charset (1B)   │  │
│  │             │  │                                    │  │
│  └─────────────┘  └────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────┘
```

### 5.1 Metadata Codes

| Code | Name | Data Format | Description |
|------|------|-------------|-------------|
| 0x01 | Title | `string` | Dictionary title |
| 0x02 | Author | `string` | Author name |
| 0x03 | Email | `string` | Author email |
| 0x04 | Copyright | `string` | Copyright information |
| 0x07 | Source Language | `uint32_t` | Source language code |
| 0x08 | Target Language | `uint32_t` | Target language code |
| 0x09 | Description | `string` | Dictionary description (with `<br>` line breaks) |
| 0x0B | Browsing Enabled | `bool` | Whether browsing is enabled |
| 0x0C | Number of Entries | `uint32_t` | Total entry count |
| 0x11 | Flags | `uint32_t` | Various flags (bit 15 = UTF-8 mode) |
| 0x14 | Creation Time | `uint32_t` | Creation timestamp |
| 0x1A | Source Charset | `uint8_t` | Source charset code |
| 0x1B | Target Charset | `uint8_t` | Target charset code |
| 0x1C | First Updated | `uint32_t` | First update timestamp |
| 0x24 | Icon2 | `binary` | Second icon data |
| 0x33 | Last Updated | `uint32_t` | Last update timestamp |
| 0x43 | Length | `uint32_t` | Substring match length |

### 5.2 Language Code Encoding

Language codes are encoded as 32-bit integers:

```c
#define BGL_LANG_CODE(index, c0, c1) \
    (((uint32_t)(index) << 16) | (((uint32_t)(c1)) << 8) | (uint32_t)(c0))

// Examples:
BGL_LANG_CODE(0, 'e', 'n') = 0x006E65  // English
BGL_LANG_CODE(0, 'f', 'r') = 0x007266  // French
BGL_LANG_CODE(0, 'i', 't') = 0x007469  // Italian
BGL_LANG_CODE(0, 'e', 's') = 0x007365  // Spanish
BGL_LANG_CODE(0, 'd', 'e') = 0x006564  // German
BGL_LANG_CODE(0, 'r', 'u') = 0x007572  // Russian
BGL_LANG_CODE(0, 'j', 'a') = 0x00616A  // Japanese
BGL_LANG_CODE(1, 'z', 'h') = 0x100687A // Traditional Chinese
BGL_LANG_CODE(2, 'z', 'h') = 0x200687A // Simplified Chinese
BGL_LANG_CODE(0, 'k', 'o') = 0x006F6B  // Korean
```

### 5.3 Charset Codes

| Code | Charset | Description |
|------|---------|-------------|
| 0x41 | WINDOWS-1252 | Default / Latin |
| 0x42 | WINDOWS-1252 | Latin |
| 0x43 | WINDOWS-1250 | Eastern European |
| 0x44 | WINDOWS-1251 | Cyrillic |
| 0x45 | CP932 | Japanese |
| 0x46 | BIG5 | Traditional Chinese |
| 0x47 | GB18030 | Simplified Chinese |
| 0x48 | CP1257 | Baltic |
| 0x49 | CP1253 | Greek |
| 0x4A | EUC-KR | Korean |
| 0x4B | ISO-8859-9 | Turkish |
| 0x4C | WINDOWS-1255 | Hebrew |
| 0x4D | CP1256 | Arabic |
| 0x4E | CP874 | Thai |

**Charset detection priority**:
1. If UTF-8 flag is set in Code 0x11 → UTF-8
2. Use charset from Code 0x1A (source) or 0x1B (target)
3. Fall back to default charset (Code 0x00, subtype 0x08)

---

## 6. Entry Blocks (Type 1, 7, 10)

These are the standard dictionary entry blocks.

```
┌─────────────────────────────────────────────────────────────┐
│              Entry Block (Type 1, 7, 10)                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌────────────┐                                             │
│  │Headword Len│  1 byte: length of headword                 │
│  ├────────────┤                                             │
│  └────────────┘                                             │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                     │
│  │        Headword (variable)         │  Headword string    │
│  └────────────────────────────────────┘                     │
│           │                                                 │
│           ▼                                                 │
│  ┌──────────────┐                                           │
│  │Defi Len (2B) │  2 bytes big-endian: definition length    │
│  ├──────────────┤                                           │
│  └──────────────┘                                           │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                     │
│  │       Definition (variable)        │  Contains special   │
│  │   (may contain embedded markers)   │  markers (see 6.1)  │
│  └────────────────────────────────────┘                     │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────┐  ┌────────────┐  ┌─────────┐                │
│  │Alt Len (1B)│ →│  Alternate │ →│   ...   │→  0x00         │
│  └────────────┘  └────────────┘  └─────────┘                │
│       Alternates list (terminated by 0x00 or block end)     │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 6.1 Special Markers in Definition

The definition field contains various special markers that must be parsed:

```
Definition byte stream:
┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
│ 0x0A │ ... │ 0x18│ 0xXX│ ... │ 0x50│ 0x1B│ 0xXX│ ... │ 0x14│ 0x02│ 0xXX│ ... │
└──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┘
   │     │     │     │     │     │     │     │     │     │     │     │
   ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼
 <br>  text  Disp  len   text  Tran  type  len   text  POS   idx   ...
              Head-              script

Marker Reference:
┌─────────────────────────────────────────────────────────────────┐
│  Marker          │ Description                                  │
├─────────────────────────────────────────────────────────────────┤
│  0x0A            │ Line break → output "<br>"                   │
│                  │                                              │
│  0x18 0xXX       │ Displayed Headword                           │
│                  │   0xXX = length, following bytes are text    │
│                  │                                              │
│  0x28 0xXXYY     │ 2-byte length Displayed Headword             │
│                  │   0XXXYY = big-endian length                 │
│                  │                                              │
│  0x40-0x5F 0x18  │ Hidden Displayed Headword                    │
│                  │   Length = (first_byte - 0x3F)               │
│                  │                                              │
│  0x50 0x1B 0xXX  │ 1-byte length transcription                  │
│                  │   0xXX = length, following bytes are text    │
│                  │   Usually Windows-1252 encoded               │
│                  │                                              │
│  0x60 0x1B 0xXXYY│ 2-byte length transcription                  │
│                  │   0XXXYY = big-endian length                 │
│                  │                                              │
│  0x1E            │ Resource reference begin marker              │
│                  │                                              │
│  0x1F            │ Resource reference end marker                │
│                  │                                              │
│  0x14 0x02 0xXX  │ Part of Speech (POS) tag                     │
│                  │   0xXX = index (0-10):                       │
│                  │     0=n., 1=adj., 2=v., 3=adv.,              │
│                  │     4=interj., 5=pron., 6=prep.,             │
│                  │     7=conj., 8=suff., 9=pref., 10=art.       │
│                  │   Output: <span class="bglpos">{tag}</span>  │
│                  │                                              │
│  0x1A 0xXX       │ Hebrew Root (if 0xXX ≤ 10)                   │
│                  │   0xXX = length, following bytes are root    │
│                  │   (bytes must be reversed)                   │
│                  │   Output: ({root})                           │
│                  │                                              │
│  0x14            │ Definition body end marker                   │
│                  │   Sets defBodyEnded = true                   │
│                  │                                              │
│  < 0x20          │ Other control bytes                          │
│                  │   Keep as-is or skip                         │
│                  │                                              │
└─────────────────────────────────────────────────────────────────┘
```

### 6.2 Part of Speech Mapping

| Index | Tag | Full Name |
|-------|-----|-----------|
| 0 | n. | Noun |
| 1 | adj. | Adjective |
| 2 | v. | Verb |
| 3 | adv. | Adverb |
| 4 | interj. | Interjection |
| 5 | pron. | Pronoun |
| 6 | prep. | Preposition |
| 7 | conj. | Conjunction |
| 8 | suff. | Suffix |
| 9 | pref. | Prefix |
| 10 | art. | Article |

---

## 7. Entry Block Variant 3 (Type 11)

Type 11 uses a **different encoding scheme** that supports more complex entries with explicit alternate counts.

```
┌─────────────────────────────────────────────────────────────┐
│                  Entry Block Type 11                         │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌────────────────┐                                        │
│  │ Word Len (4B)  │  4 bytes big-endian: headword length    │
│  ├────────────────┤                                        │
│  └────────────────┘                                        │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                    │
│  │         Headword (variable)        │                    │
│  └────────────────────────────────────┘                    │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────┐                                        │
│  │Alt Count (4B) │  4 bytes big-endian: alternate count    │
│  ├────────────────┤                                        │
│  └────────────────┘                                        │
│           │                                                 │
│           ▼                                                 │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐                     │
│  │ Alt Len │→│ Altword │→│   ...   │  Repeated Alt Count   │
│  │  (4B)   │  │(variable│  │         │  times               │
│  └─────────┘  └─────────┘  └─────────┘                     │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────┐                                        │
│  │Defi Len (4B)   │  4 bytes big-endian: definition length │
│  ├────────────────┤                                        │
│  └────────────────┘                                        │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                    │
│  │       Definition (variable)       │  Contains special    │
│  └────────────────────────────────────┘  markers            │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### 7.1 Type 11 vs Type 1 Differences

| Feature | Type 1/7/10 | Type 11 |
|---------|-------------|---------|
| Headword Length | 1 byte | 4 bytes |
| Alternate Position | After definition | After headword |
| Alternate Length | 1 byte | 4 bytes |
| Definition Length | 2 bytes | 4 bytes |
| Alternate Count | Implicit (0-terminated) | Explicit count |

---

## 8. Resource Blocks (Type 2)

Resource blocks store embedded files (images, HTML, etc.):

```
┌─────────────────────────────────────────────────────────────┐
│                   Resource Block (Type 2)                    │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌────────────┐                                             │
│  │Name Len (1B)│  1 byte: resource name length              │
│  ├────────────┤                                             │
│  └────────────┘                                             │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                    │
│  │      Resource Name (variable)      │  e.g., "image.png"   │
│  └────────────────────────────────────┘                    │
│           │                                                 │
│           ▼                                                 │
│  ┌────────────────────────────────────┐                    │
│  │      Resource Data (remainder)     │  Binary data        │
│  └────────────────────────────────────┘                    │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

---

## 9. Charset Tags

Definitions may contain `<charset>` tags to specify different encodings:

```
┌─────────────────────────────────────────────────────────────┐
│                    Charset Tag Formats                       │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  <charset c="U">content</charset>                             │
│  └── UTF-8 encoded content                                   │
│                                                               │
│  <charset c="K">content</charset>                             │
│  └── Source language encoded content                         │
│                                                               │
│  <charset c="E">content</charset>                             │
│  └── Source language encoded content                         │
│                                                               │
│  <charset c="G">content</charset>                             │
│  └── GBK encoded content                                     │
│                                                               │
│  <charset c="T">00E6;00E9;</charset>                         │
│  └── Babylon character references (hex Unicode points)        │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

---

## 10. Parsing Workflow

```
┌─────────────────────────────────────────────────────────────┐
│                    BGL Parsing Workflow                      │
└─────────────────────────────────────────────────────────────┘

Step 1: Read File Header
├─ Read 6 bytes
├─ Verify signature (0x12340001/0x12340002)
└─ Get gzip offset

Step 2: Open Gzip Stream
├─ Use dup + lseek for cross-platform compatibility
└─ gzdopen() to open

Step 3: First Pass - Collect Metadata
├─ Loop through all blocks
├─ Type 0: Parse charset information
├─ Type 3: Parse metadata (title/author/languages/etc.)
├─ Type 1/7/10/11: Count entries
└─ gzrewind() to reset position

Step 4: Second Pass - Read Entries
├─ Loop through blocks
├─ Type 1/7/10/11: Parse entry
│  ├─ Read Headword
│  ├─ Read Definition (process special markers)
│  ├─ Read Alternates
│  └─ Convert to UTF-8
├─ Type 2: Process resources
└─ Type 4: EOF, exit

Step 5: Build Index (optional)
├─ Build B-tree index
├─ Save to .idx file
└─ Use index for subsequent access
```

---

## 11. Implementation Notes

### 11.1 Two-Pass Requirement

BGL files require two full passes through the gzip stream:

1. **First pass**: Scan for metadata (Type 3 blocks) and detect encoding
2. **Second pass**: Read and process entries (Type 1/7/10/11 blocks)

This is necessary because:
- Metadata blocks can appear anywhere in the file
- Encoding information must be known before decoding entries
- Entry counts are accumulated during scanning

### 11.2 Optimization Strategies

**For one-time processing**:
- Merge both passes into a single scan
- Process entries immediately after metadata is complete
- Build external index during the scan

**For repeated access**:
- Build and persist an index file (like GoldenDict's .idx)
- Store entries in a more efficient format (SQLite, custom index, etc.)
- Never parse the BGL file again after indexing

### 11.3 Character Encoding Handling

```c
// Charset selection priority
if (utf8_flag) {
    charset = "UTF-8";
} else if (source_charset_from_metadata) {
    charset = source_charset_from_metadata;
} else {
    charset = default_charset;  // Usually WINDOWS-1252
}
```

### 11.4 Special Marker Handling

Definition text contains binary control codes that must be handled carefully:

```c
for (size_t i = 0; i < definition_len; i++) {
    uint8_t b = definition[i];

    if (b == 0x0A) {
        // Line break
        output += "<br>";
    } else if (b == 0x18 && i + 1 < definition_len) {
        // Displayed headword
        uint8_t len = definition[++i];
        // Process next 'len' bytes as displayed headword
    } else if (b == 0x14 && i + 2 < definition_len) {
        if (definition[i + 1] == 0x02) {
            // POS tag
            uint8_t idx = definition[i + 2] - '0';
            output += pos_tags[idx];
            i += 2;
        }
    }
    // ... handle other markers
}
```

### 11.5 Cross-Platform Gzip Handling

```c
// macOS requires this approach
int fd = dup(fileno(fp));
lseek(fd, gzip_offset, SEEK_SET);
gzFile gzf = gzdopen(fd, "rb");
fclose(fp);  // Close original, gzf now owns the fd
```

---

## 12. References

- **GoldenDict**: https://github.com/goldendict/goldendict
  - Source: `src/dict/bgl_babylon.cc`, `src/dict/bgl.cc`

- **pyglossary**: https://github.com/ilius/pyglossary
  - Source: `pyglossary/babylon_bgl.py`

- **BGL Reader** (this implementation):
  - Source: `src/formats/babylon/`

---

*Document Version: 1.0*
*Last Updated: 2026-03-12*

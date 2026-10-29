# On-disk formats

All integer endianness and record framing below describe the current format;
there is no backward-compatibility guarantee yet.

## WAL and MANIFEST physical records

WAL and MANIFEST use the same physical format. Files are divided into 32 KiB
blocks. Each record fragment has a 7-byte header:

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 4 | CRC32C, little-endian, over type byte plus payload |
| 4 | 2 | Payload length, little-endian |
| 6 | 1 | Fragment type: FULL=1, FIRST=2, MIDDLE=3, LAST=4 |
| 7 | length | Fragment payload |

Fragments never cross a block boundary. Zero padding is used when fewer than
7 bytes remain. Recovery retains complete logical records and truncates at the
first invalid or torn suffix.

## DB write-group payload

The WAL payload starts with fixed32 little-endian format version `1`, followed
by a varint operation count. Each operation has fixed64 little-endian sequence,
one value-type byte (`0` deletion, `1` value), varint-length-prefixed key, and,
for values only, varint-length-prefixed value.

## SSTable blocks and footer

Each data, index, and filter block is followed by a fixed32 little-endian
CRC32C over the block contents. A block entry contains varint shared-key
length, varint non-shared-key length, varint value length, non-shared key
bytes, and value bytes. The end of the block contains fixed32 little-endian
restart offsets followed by a fixed32 restart count.

The final 40-byte footer has five little-endian fixed64 fields:

| Offset | Width | Meaning |
|---:|---:|---|
| 0 | 8 | Magic `0x3153464f5243494d` |
| 8 | 8 | Index block offset |
| 16 | 8 | Index block content size |
| 24 | 8 | Filter block offset |
| 32 | 8 | Filter block content size |

The magic's on-disk bytes spell `MICROFS1`. Block handles exclude the trailing
4-byte checksum. The index maps each data block's last key to its handle. The
filter block contains fixed32 version and prefix length, then length-prefixed
key and prefix Bloom filter encodings.

## Internal keys

The user-key bytes are copied, with each `00` byte escaped as `00 FF`; `00 00`
terminates the user key. The 8-byte big-endian suffix stores
`((max_sequence - sequence) << 8) | type`, making bytewise sort order
user-key ascending, sequence descending, then type ascending.

## CURRENT and MANIFEST edit payload

CURRENT is the active `MANIFEST-<decimal file number>` name followed by a
newline. A MANIFEST payload starts with fixed32 little-endian format version
`1`, fixed64 last sequence, fixed64 next file number, and a varint added-file
count. Each added file stores fixed32 level, fixed64 file number and size, and
varint-length-prefixed smallest/largest internal keys. A varint deletion count
then precedes `(fixed32 level, fixed64 file number)` deletion entries. Payloads
are WAL-framed and checksummed.

## Tick keys and values

Tick keys begin with the reserved five-byte `01 54 49 43 4B` namespace, then
the zero-escaped symbol (`00` becomes `00 FF`), `00 00`, and a big-endian
uint64 timestamp in nanoseconds. A value starts with a varint tick count; the
first tick stores zig-zag varint absolute signed price and varint size.
Subsequent ticks store a varint timestamp delta, zig-zag varint price delta,
and varint size.

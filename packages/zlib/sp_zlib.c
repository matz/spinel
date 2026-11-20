/* sp_zlib.c -- DEFLATE (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952) for
   the `zlib` spin package (Path B carried C), linked on demand when
   `require "zlib"` appears.

   WHY THIS IS NOT A BINDING TO THE SYSTEM libz. packages/openssl is glue over
   the system libssl on purpose: TLS has trust anchors and a protocol clock,
   and both belong to the distribution. Decompression has neither. What it has
   instead is a host problem: a machine with libz.so.1 and no zlib.h -- an
   ordinary developer box without the -dev package, which is where this was
   written -- would fail the availability probe, drop the package, and give a
   green `make test` that never ran a line of it. That is the exact failure
   the openssl probe's comment is about. spinel already carries its own regexp
   engine and bigint rather than depending on the host's; this is the same
   answer for the same reason.

   Inflate is complete: stored, fixed-Huffman and dynamic-Huffman blocks, with
   the zlib and gzip wrappers and their checksums. Deflate is LZ77 with a hash
   chain emitting fixed-Huffman blocks -- a valid stream any inflater reads,
   at a ratio below zlib's own, which builds dynamic tables per block.
   Both also work on a stream in pieces, with no state kept here. Deflate
   writes a block per flush, and inflate reads as far as the input it is
   given goes and says where it stopped: the Ruby side keeps the wrapper,
   the checksum, the input still to read and the window.

   Binary safe throughout: input length comes from the string header
   (sp_str_byte_len), output is a length-set BINARY string on the GC heap.

   Every symbol here carries the full package prefix, including the static
   helpers, and that is not tidiness. The build derives its reserved-identifier
   list from the FIRST underscore component of every runtime name in lib/ and
   packages/, so a helper whose component was "z" reserved that letter and
   renamed a user method called `z` in every program that never heard of this
   package. The generated-C sweep is what showed it: two unrelated tests moved
   to the escaped spelling. One prefix in, one prefix claimed.

   The same rule is why no identifier is spelled out in this comment: the
   extraction greps the file, comments and all, so naming an example here
   would reserve the example. */
#include "spinel/runtime.h"
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- checksums */

/* CRC-32 (reflected polynomial 0xEDB88320), eight bytes a step. Row 0 is the
   table of the one-byte step; row k is the CRC of each byte value followed
   by k zero bytes, which lets the eight bytes of a step be looked up side
   by side and XORed:

     row[k][i] = (row[k-1][i] >> 8) ^ row[0][row[k-1][i] & 0xff]

   The rows are constants and not filled in on first use: threads run in
   parallel here, and a table built lazily is one more thing they would
   have to take turns on. */
static const uint32_t sp_zlib_crc_table[8][256] = {
  {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu,
    0xe963a535u, 0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u,
    0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u,
    0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u,
    0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u,
    0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
    0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u,
    0xcfba9599u, 0xb8bda50fu, 0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u,
    0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du, 0x76dc4190u, 0x01db7106u,
    0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du,
    0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u,
    0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u,
    0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u,
    0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u, 0x5005713cu, 0x270241aau,
    0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
    0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au,
    0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u,
    0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu,
    0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu,
    0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u, 0xa1d1937eu,
    0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u,
    0x316e8eefu, 0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u,
    0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u,
    0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu,
    0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u,
    0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u,
    0x616bffd3u, 0x166ccf45u, 0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u,
    0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu,
    0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u,
    0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du
  },
  {
    0x00000000u, 0x191b3141u, 0x32366282u, 0x2b2d53c3u, 0x646cc504u, 0x7d77f445u,
    0x565aa786u, 0x4f4196c7u, 0xc8d98a08u, 0xd1c2bb49u, 0xfaefe88au, 0xe3f4d9cbu,
    0xacb54f0cu, 0xb5ae7e4du, 0x9e832d8eu, 0x87981ccfu, 0x4ac21251u, 0x53d92310u,
    0x78f470d3u, 0x61ef4192u, 0x2eaed755u, 0x37b5e614u, 0x1c98b5d7u, 0x05838496u,
    0x821b9859u, 0x9b00a918u, 0xb02dfadbu, 0xa936cb9au, 0xe6775d5du, 0xff6c6c1cu,
    0xd4413fdfu, 0xcd5a0e9eu, 0x958424a2u, 0x8c9f15e3u, 0xa7b24620u, 0xbea97761u,
    0xf1e8e1a6u, 0xe8f3d0e7u, 0xc3de8324u, 0xdac5b265u, 0x5d5daeaau, 0x44469febu,
    0x6f6bcc28u, 0x7670fd69u, 0x39316baeu, 0x202a5aefu, 0x0b07092cu, 0x121c386du,
    0xdf4636f3u, 0xc65d07b2u, 0xed705471u, 0xf46b6530u, 0xbb2af3f7u, 0xa231c2b6u,
    0x891c9175u, 0x9007a034u, 0x179fbcfbu, 0x0e848dbau, 0x25a9de79u, 0x3cb2ef38u,
    0x73f379ffu, 0x6ae848beu, 0x41c51b7du, 0x58de2a3cu, 0xf0794f05u, 0xe9627e44u,
    0xc24f2d87u, 0xdb541cc6u, 0x94158a01u, 0x8d0ebb40u, 0xa623e883u, 0xbf38d9c2u,
    0x38a0c50du, 0x21bbf44cu, 0x0a96a78fu, 0x138d96ceu, 0x5ccc0009u, 0x45d73148u,
    0x6efa628bu, 0x77e153cau, 0xbabb5d54u, 0xa3a06c15u, 0x888d3fd6u, 0x91960e97u,
    0xded79850u, 0xc7cca911u, 0xece1fad2u, 0xf5facb93u, 0x7262d75cu, 0x6b79e61du,
    0x4054b5deu, 0x594f849fu, 0x160e1258u, 0x0f152319u, 0x243870dau, 0x3d23419bu,
    0x65fd6ba7u, 0x7ce65ae6u, 0x57cb0925u, 0x4ed03864u, 0x0191aea3u, 0x188a9fe2u,
    0x33a7cc21u, 0x2abcfd60u, 0xad24e1afu, 0xb43fd0eeu, 0x9f12832du, 0x8609b26cu,
    0xc94824abu, 0xd05315eau, 0xfb7e4629u, 0xe2657768u, 0x2f3f79f6u, 0x362448b7u,
    0x1d091b74u, 0x04122a35u, 0x4b53bcf2u, 0x52488db3u, 0x7965de70u, 0x607eef31u,
    0xe7e6f3feu, 0xfefdc2bfu, 0xd5d0917cu, 0xcccba03du, 0x838a36fau, 0x9a9107bbu,
    0xb1bc5478u, 0xa8a76539u, 0x3b83984bu, 0x2298a90au, 0x09b5fac9u, 0x10aecb88u,
    0x5fef5d4fu, 0x46f46c0eu, 0x6dd93fcdu, 0x74c20e8cu, 0xf35a1243u, 0xea412302u,
    0xc16c70c1u, 0xd8774180u, 0x9736d747u, 0x8e2de606u, 0xa500b5c5u, 0xbc1b8484u,
    0x71418a1au, 0x685abb5bu, 0x4377e898u, 0x5a6cd9d9u, 0x152d4f1eu, 0x0c367e5fu,
    0x271b2d9cu, 0x3e001cddu, 0xb9980012u, 0xa0833153u, 0x8bae6290u, 0x92b553d1u,
    0xddf4c516u, 0xc4eff457u, 0xefc2a794u, 0xf6d996d5u, 0xae07bce9u, 0xb71c8da8u,
    0x9c31de6bu, 0x852aef2au, 0xca6b79edu, 0xd37048acu, 0xf85d1b6fu, 0xe1462a2eu,
    0x66de36e1u, 0x7fc507a0u, 0x54e85463u, 0x4df36522u, 0x02b2f3e5u, 0x1ba9c2a4u,
    0x30849167u, 0x299fa026u, 0xe4c5aeb8u, 0xfdde9ff9u, 0xd6f3cc3au, 0xcfe8fd7bu,
    0x80a96bbcu, 0x99b25afdu, 0xb29f093eu, 0xab84387fu, 0x2c1c24b0u, 0x350715f1u,
    0x1e2a4632u, 0x07317773u, 0x4870e1b4u, 0x516bd0f5u, 0x7a468336u, 0x635db277u,
    0xcbfad74eu, 0xd2e1e60fu, 0xf9ccb5ccu, 0xe0d7848du, 0xaf96124au, 0xb68d230bu,
    0x9da070c8u, 0x84bb4189u, 0x03235d46u, 0x1a386c07u, 0x31153fc4u, 0x280e0e85u,
    0x674f9842u, 0x7e54a903u, 0x5579fac0u, 0x4c62cb81u, 0x8138c51fu, 0x9823f45eu,
    0xb30ea79du, 0xaa1596dcu, 0xe554001bu, 0xfc4f315au, 0xd7626299u, 0xce7953d8u,
    0x49e14f17u, 0x50fa7e56u, 0x7bd72d95u, 0x62cc1cd4u, 0x2d8d8a13u, 0x3496bb52u,
    0x1fbbe891u, 0x06a0d9d0u, 0x5e7ef3ecu, 0x4765c2adu, 0x6c48916eu, 0x7553a02fu,
    0x3a1236e8u, 0x230907a9u, 0x0824546au, 0x113f652bu, 0x96a779e4u, 0x8fbc48a5u,
    0xa4911b66u, 0xbd8a2a27u, 0xf2cbbce0u, 0xebd08da1u, 0xc0fdde62u, 0xd9e6ef23u,
    0x14bce1bdu, 0x0da7d0fcu, 0x268a833fu, 0x3f91b27eu, 0x70d024b9u, 0x69cb15f8u,
    0x42e6463bu, 0x5bfd777au, 0xdc656bb5u, 0xc57e5af4u, 0xee530937u, 0xf7483876u,
    0xb809aeb1u, 0xa1129ff0u, 0x8a3fcc33u, 0x9324fd72u
  },
  {
    0x00000000u, 0x01c26a37u, 0x0384d46eu, 0x0246be59u, 0x0709a8dcu, 0x06cbc2ebu,
    0x048d7cb2u, 0x054f1685u, 0x0e1351b8u, 0x0fd13b8fu, 0x0d9785d6u, 0x0c55efe1u,
    0x091af964u, 0x08d89353u, 0x0a9e2d0au, 0x0b5c473du, 0x1c26a370u, 0x1de4c947u,
    0x1fa2771eu, 0x1e601d29u, 0x1b2f0bacu, 0x1aed619bu, 0x18abdfc2u, 0x1969b5f5u,
    0x1235f2c8u, 0x13f798ffu, 0x11b126a6u, 0x10734c91u, 0x153c5a14u, 0x14fe3023u,
    0x16b88e7au, 0x177ae44du, 0x384d46e0u, 0x398f2cd7u, 0x3bc9928eu, 0x3a0bf8b9u,
    0x3f44ee3cu, 0x3e86840bu, 0x3cc03a52u, 0x3d025065u, 0x365e1758u, 0x379c7d6fu,
    0x35dac336u, 0x3418a901u, 0x3157bf84u, 0x3095d5b3u, 0x32d36beau, 0x331101ddu,
    0x246be590u, 0x25a98fa7u, 0x27ef31feu, 0x262d5bc9u, 0x23624d4cu, 0x22a0277bu,
    0x20e69922u, 0x2124f315u, 0x2a78b428u, 0x2bbade1fu, 0x29fc6046u, 0x283e0a71u,
    0x2d711cf4u, 0x2cb376c3u, 0x2ef5c89au, 0x2f37a2adu, 0x709a8dc0u, 0x7158e7f7u,
    0x731e59aeu, 0x72dc3399u, 0x7793251cu, 0x76514f2bu, 0x7417f172u, 0x75d59b45u,
    0x7e89dc78u, 0x7f4bb64fu, 0x7d0d0816u, 0x7ccf6221u, 0x798074a4u, 0x78421e93u,
    0x7a04a0cau, 0x7bc6cafdu, 0x6cbc2eb0u, 0x6d7e4487u, 0x6f38fadeu, 0x6efa90e9u,
    0x6bb5866cu, 0x6a77ec5bu, 0x68315202u, 0x69f33835u, 0x62af7f08u, 0x636d153fu,
    0x612bab66u, 0x60e9c151u, 0x65a6d7d4u, 0x6464bde3u, 0x662203bau, 0x67e0698du,
    0x48d7cb20u, 0x4915a117u, 0x4b531f4eu, 0x4a917579u, 0x4fde63fcu, 0x4e1c09cbu,
    0x4c5ab792u, 0x4d98dda5u, 0x46c49a98u, 0x4706f0afu, 0x45404ef6u, 0x448224c1u,
    0x41cd3244u, 0x400f5873u, 0x4249e62au, 0x438b8c1du, 0x54f16850u, 0x55330267u,
    0x5775bc3eu, 0x56b7d609u, 0x53f8c08cu, 0x523aaabbu, 0x507c14e2u, 0x51be7ed5u,
    0x5ae239e8u, 0x5b2053dfu, 0x5966ed86u, 0x58a487b1u, 0x5deb9134u, 0x5c29fb03u,
    0x5e6f455au, 0x5fad2f6du, 0xe1351b80u, 0xe0f771b7u, 0xe2b1cfeeu, 0xe373a5d9u,
    0xe63cb35cu, 0xe7fed96bu, 0xe5b86732u, 0xe47a0d05u, 0xef264a38u, 0xeee4200fu,
    0xeca29e56u, 0xed60f461u, 0xe82fe2e4u, 0xe9ed88d3u, 0xebab368au, 0xea695cbdu,
    0xfd13b8f0u, 0xfcd1d2c7u, 0xfe976c9eu, 0xff5506a9u, 0xfa1a102cu, 0xfbd87a1bu,
    0xf99ec442u, 0xf85cae75u, 0xf300e948u, 0xf2c2837fu, 0xf0843d26u, 0xf1465711u,
    0xf4094194u, 0xf5cb2ba3u, 0xf78d95fau, 0xf64fffcdu, 0xd9785d60u, 0xd8ba3757u,
    0xdafc890eu, 0xdb3ee339u, 0xde71f5bcu, 0xdfb39f8bu, 0xddf521d2u, 0xdc374be5u,
    0xd76b0cd8u, 0xd6a966efu, 0xd4efd8b6u, 0xd52db281u, 0xd062a404u, 0xd1a0ce33u,
    0xd3e6706au, 0xd2241a5du, 0xc55efe10u, 0xc49c9427u, 0xc6da2a7eu, 0xc7184049u,
    0xc25756ccu, 0xc3953cfbu, 0xc1d382a2u, 0xc011e895u, 0xcb4dafa8u, 0xca8fc59fu,
    0xc8c97bc6u, 0xc90b11f1u, 0xcc440774u, 0xcd866d43u, 0xcfc0d31au, 0xce02b92du,
    0x91af9640u, 0x906dfc77u, 0x922b422eu, 0x93e92819u, 0x96a63e9cu, 0x976454abu,
    0x9522eaf2u, 0x94e080c5u, 0x9fbcc7f8u, 0x9e7eadcfu, 0x9c381396u, 0x9dfa79a1u,
    0x98b56f24u, 0x99770513u, 0x9b31bb4au, 0x9af3d17du, 0x8d893530u, 0x8c4b5f07u,
    0x8e0de15eu, 0x8fcf8b69u, 0x8a809decu, 0x8b42f7dbu, 0x89044982u, 0x88c623b5u,
    0x839a6488u, 0x82580ebfu, 0x801eb0e6u, 0x81dcdad1u, 0x8493cc54u, 0x8551a663u,
    0x8717183au, 0x86d5720du, 0xa9e2d0a0u, 0xa820ba97u, 0xaa6604ceu, 0xaba46ef9u,
    0xaeeb787cu, 0xaf29124bu, 0xad6fac12u, 0xacadc625u, 0xa7f18118u, 0xa633eb2fu,
    0xa4755576u, 0xa5b73f41u, 0xa0f829c4u, 0xa13a43f3u, 0xa37cfdaau, 0xa2be979du,
    0xb5c473d0u, 0xb40619e7u, 0xb640a7beu, 0xb782cd89u, 0xb2cddb0cu, 0xb30fb13bu,
    0xb1490f62u, 0xb08b6555u, 0xbbd72268u, 0xba15485fu, 0xb853f606u, 0xb9919c31u,
    0xbcde8ab4u, 0xbd1ce083u, 0xbf5a5edau, 0xbe9834edu
  },
  {
    0x00000000u, 0xb8bc6765u, 0xaa09c88bu, 0x12b5afeeu, 0x8f629757u, 0x37def032u,
    0x256b5fdcu, 0x9dd738b9u, 0xc5b428efu, 0x7d084f8au, 0x6fbde064u, 0xd7018701u,
    0x4ad6bfb8u, 0xf26ad8ddu, 0xe0df7733u, 0x58631056u, 0x5019579fu, 0xe8a530fau,
    0xfa109f14u, 0x42acf871u, 0xdf7bc0c8u, 0x67c7a7adu, 0x75720843u, 0xcdce6f26u,
    0x95ad7f70u, 0x2d111815u, 0x3fa4b7fbu, 0x8718d09eu, 0x1acfe827u, 0xa2738f42u,
    0xb0c620acu, 0x087a47c9u, 0xa032af3eu, 0x188ec85bu, 0x0a3b67b5u, 0xb28700d0u,
    0x2f503869u, 0x97ec5f0cu, 0x8559f0e2u, 0x3de59787u, 0x658687d1u, 0xdd3ae0b4u,
    0xcf8f4f5au, 0x7733283fu, 0xeae41086u, 0x525877e3u, 0x40edd80du, 0xf851bf68u,
    0xf02bf8a1u, 0x48979fc4u, 0x5a22302au, 0xe29e574fu, 0x7f496ff6u, 0xc7f50893u,
    0xd540a77du, 0x6dfcc018u, 0x359fd04eu, 0x8d23b72bu, 0x9f9618c5u, 0x272a7fa0u,
    0xbafd4719u, 0x0241207cu, 0x10f48f92u, 0xa848e8f7u, 0x9b14583du, 0x23a83f58u,
    0x311d90b6u, 0x89a1f7d3u, 0x1476cf6au, 0xaccaa80fu, 0xbe7f07e1u, 0x06c36084u,
    0x5ea070d2u, 0xe61c17b7u, 0xf4a9b859u, 0x4c15df3cu, 0xd1c2e785u, 0x697e80e0u,
    0x7bcb2f0eu, 0xc377486bu, 0xcb0d0fa2u, 0x73b168c7u, 0x6104c729u, 0xd9b8a04cu,
    0x446f98f5u, 0xfcd3ff90u, 0xee66507eu, 0x56da371bu, 0x0eb9274du, 0xb6054028u,
    0xa4b0efc6u, 0x1c0c88a3u, 0x81dbb01au, 0x3967d77fu, 0x2bd27891u, 0x936e1ff4u,
    0x3b26f703u, 0x839a9066u, 0x912f3f88u, 0x299358edu, 0xb4446054u, 0x0cf80731u,
    0x1e4da8dfu, 0xa6f1cfbau, 0xfe92dfecu, 0x462eb889u, 0x549b1767u, 0xec277002u,
    0x71f048bbu, 0xc94c2fdeu, 0xdbf98030u, 0x6345e755u, 0x6b3fa09cu, 0xd383c7f9u,
    0xc1366817u, 0x798a0f72u, 0xe45d37cbu, 0x5ce150aeu, 0x4e54ff40u, 0xf6e89825u,
    0xae8b8873u, 0x1637ef16u, 0x048240f8u, 0xbc3e279du, 0x21e91f24u, 0x99557841u,
    0x8be0d7afu, 0x335cb0cau, 0xed59b63bu, 0x55e5d15eu, 0x47507eb0u, 0xffec19d5u,
    0x623b216cu, 0xda874609u, 0xc832e9e7u, 0x708e8e82u, 0x28ed9ed4u, 0x9051f9b1u,
    0x82e4565fu, 0x3a58313au, 0xa78f0983u, 0x1f336ee6u, 0x0d86c108u, 0xb53aa66du,
    0xbd40e1a4u, 0x05fc86c1u, 0x1749292fu, 0xaff54e4au, 0x322276f3u, 0x8a9e1196u,
    0x982bbe78u, 0x2097d91du, 0x78f4c94bu, 0xc048ae2eu, 0xd2fd01c0u, 0x6a4166a5u,
    0xf7965e1cu, 0x4f2a3979u, 0x5d9f9697u, 0xe523f1f2u, 0x4d6b1905u, 0xf5d77e60u,
    0xe762d18eu, 0x5fdeb6ebu, 0xc2098e52u, 0x7ab5e937u, 0x680046d9u, 0xd0bc21bcu,
    0x88df31eau, 0x3063568fu, 0x22d6f961u, 0x9a6a9e04u, 0x07bda6bdu, 0xbf01c1d8u,
    0xadb46e36u, 0x15080953u, 0x1d724e9au, 0xa5ce29ffu, 0xb77b8611u, 0x0fc7e174u,
    0x9210d9cdu, 0x2aacbea8u, 0x38191146u, 0x80a57623u, 0xd8c66675u, 0x607a0110u,
    0x72cfaefeu, 0xca73c99bu, 0x57a4f122u, 0xef189647u, 0xfdad39a9u, 0x45115eccu,
    0x764dee06u, 0xcef18963u, 0xdc44268du, 0x64f841e8u, 0xf92f7951u, 0x41931e34u,
    0x5326b1dau, 0xeb9ad6bfu, 0xb3f9c6e9u, 0x0b45a18cu, 0x19f00e62u, 0xa14c6907u,
    0x3c9b51beu, 0x842736dbu, 0x96929935u, 0x2e2efe50u, 0x2654b999u, 0x9ee8defcu,
    0x8c5d7112u, 0x34e11677u, 0xa9362eceu, 0x118a49abu, 0x033fe645u, 0xbb838120u,
    0xe3e09176u, 0x5b5cf613u, 0x49e959fdu, 0xf1553e98u, 0x6c820621u, 0xd43e6144u,
    0xc68bceaau, 0x7e37a9cfu, 0xd67f4138u, 0x6ec3265du, 0x7c7689b3u, 0xc4caeed6u,
    0x591dd66fu, 0xe1a1b10au, 0xf3141ee4u, 0x4ba87981u, 0x13cb69d7u, 0xab770eb2u,
    0xb9c2a15cu, 0x017ec639u, 0x9ca9fe80u, 0x241599e5u, 0x36a0360bu, 0x8e1c516eu,
    0x866616a7u, 0x3eda71c2u, 0x2c6fde2cu, 0x94d3b949u, 0x090481f0u, 0xb1b8e695u,
    0xa30d497bu, 0x1bb12e1eu, 0x43d23e48u, 0xfb6e592du, 0xe9dbf6c3u, 0x516791a6u,
    0xccb0a91fu, 0x740cce7au, 0x66b96194u, 0xde0506f1u
  },
  {
    0x00000000u, 0x3d6029b0u, 0x7ac05360u, 0x47a07ad0u, 0xf580a6c0u, 0xc8e08f70u,
    0x8f40f5a0u, 0xb220dc10u, 0x30704bc1u, 0x0d106271u, 0x4ab018a1u, 0x77d03111u,
    0xc5f0ed01u, 0xf890c4b1u, 0xbf30be61u, 0x825097d1u, 0x60e09782u, 0x5d80be32u,
    0x1a20c4e2u, 0x2740ed52u, 0x95603142u, 0xa80018f2u, 0xefa06222u, 0xd2c04b92u,
    0x5090dc43u, 0x6df0f5f3u, 0x2a508f23u, 0x1730a693u, 0xa5107a83u, 0x98705333u,
    0xdfd029e3u, 0xe2b00053u, 0xc1c12f04u, 0xfca106b4u, 0xbb017c64u, 0x866155d4u,
    0x344189c4u, 0x0921a074u, 0x4e81daa4u, 0x73e1f314u, 0xf1b164c5u, 0xccd14d75u,
    0x8b7137a5u, 0xb6111e15u, 0x0431c205u, 0x3951ebb5u, 0x7ef19165u, 0x4391b8d5u,
    0xa121b886u, 0x9c419136u, 0xdbe1ebe6u, 0xe681c256u, 0x54a11e46u, 0x69c137f6u,
    0x2e614d26u, 0x13016496u, 0x9151f347u, 0xac31daf7u, 0xeb91a027u, 0xd6f18997u,
    0x64d15587u, 0x59b17c37u, 0x1e1106e7u, 0x23712f57u, 0x58f35849u, 0x659371f9u,
    0x22330b29u, 0x1f532299u, 0xad73fe89u, 0x9013d739u, 0xd7b3ade9u, 0xead38459u,
    0x68831388u, 0x55e33a38u, 0x124340e8u, 0x2f236958u, 0x9d03b548u, 0xa0639cf8u,
    0xe7c3e628u, 0xdaa3cf98u, 0x3813cfcbu, 0x0573e67bu, 0x42d39cabu, 0x7fb3b51bu,
    0xcd93690bu, 0xf0f340bbu, 0xb7533a6bu, 0x8a3313dbu, 0x0863840au, 0x3503adbau,
    0x72a3d76au, 0x4fc3fedau, 0xfde322cau, 0xc0830b7au, 0x872371aau, 0xba43581au,
    0x9932774du, 0xa4525efdu, 0xe3f2242du, 0xde920d9du, 0x6cb2d18du, 0x51d2f83du,
    0x167282edu, 0x2b12ab5du, 0xa9423c8cu, 0x9422153cu, 0xd3826fecu, 0xeee2465cu,
    0x5cc29a4cu, 0x61a2b3fcu, 0x2602c92cu, 0x1b62e09cu, 0xf9d2e0cfu, 0xc4b2c97fu,
    0x8312b3afu, 0xbe729a1fu, 0x0c52460fu, 0x31326fbfu, 0x7692156fu, 0x4bf23cdfu,
    0xc9a2ab0eu, 0xf4c282beu, 0xb362f86eu, 0x8e02d1deu, 0x3c220dceu, 0x0142247eu,
    0x46e25eaeu, 0x7b82771eu, 0xb1e6b092u, 0x8c869922u, 0xcb26e3f2u, 0xf646ca42u,
    0x44661652u, 0x79063fe2u, 0x3ea64532u, 0x03c66c82u, 0x8196fb53u, 0xbcf6d2e3u,
    0xfb56a833u, 0xc6368183u, 0x74165d93u, 0x49767423u, 0x0ed60ef3u, 0x33b62743u,
    0xd1062710u, 0xec660ea0u, 0xabc67470u, 0x96a65dc0u, 0x248681d0u, 0x19e6a860u,
    0x5e46d2b0u, 0x6326fb00u, 0xe1766cd1u, 0xdc164561u, 0x9bb63fb1u, 0xa6d61601u,
    0x14f6ca11u, 0x2996e3a1u, 0x6e369971u, 0x5356b0c1u, 0x70279f96u, 0x4d47b626u,
    0x0ae7ccf6u, 0x3787e546u, 0x85a73956u, 0xb8c710e6u, 0xff676a36u, 0xc2074386u,
    0x4057d457u, 0x7d37fde7u, 0x3a978737u, 0x07f7ae87u, 0xb5d77297u, 0x88b75b27u,
    0xcf1721f7u, 0xf2770847u, 0x10c70814u, 0x2da721a4u, 0x6a075b74u, 0x576772c4u,
    0xe547aed4u, 0xd8278764u, 0x9f87fdb4u, 0xa2e7d404u, 0x20b743d5u, 0x1dd76a65u,
    0x5a7710b5u, 0x67173905u, 0xd537e515u, 0xe857cca5u, 0xaff7b675u, 0x92979fc5u,
    0xe915e8dbu, 0xd475c16bu, 0x93d5bbbbu, 0xaeb5920bu, 0x1c954e1bu, 0x21f567abu,
    0x66551d7bu, 0x5b3534cbu, 0xd965a31au, 0xe4058aaau, 0xa3a5f07au, 0x9ec5d9cau,
    0x2ce505dau, 0x11852c6au, 0x562556bau, 0x6b457f0au, 0x89f57f59u, 0xb49556e9u,
    0xf3352c39u, 0xce550589u, 0x7c75d999u, 0x4115f029u, 0x06b58af9u, 0x3bd5a349u,
    0xb9853498u, 0x84e51d28u, 0xc34567f8u, 0xfe254e48u, 0x4c059258u, 0x7165bbe8u,
    0x36c5c138u, 0x0ba5e888u, 0x28d4c7dfu, 0x15b4ee6fu, 0x521494bfu, 0x6f74bd0fu,
    0xdd54611fu, 0xe03448afu, 0xa794327fu, 0x9af41bcfu, 0x18a48c1eu, 0x25c4a5aeu,
    0x6264df7eu, 0x5f04f6ceu, 0xed242adeu, 0xd044036eu, 0x97e479beu, 0xaa84500eu,
    0x4834505du, 0x755479edu, 0x32f4033du, 0x0f942a8du, 0xbdb4f69du, 0x80d4df2du,
    0xc774a5fdu, 0xfa148c4du, 0x78441b9cu, 0x4524322cu, 0x028448fcu, 0x3fe4614cu,
    0x8dc4bd5cu, 0xb0a494ecu, 0xf704ee3cu, 0xca64c78cu
  },
  {
    0x00000000u, 0xcb5cd3a5u, 0x4dc8a10bu, 0x869472aeu, 0x9b914216u, 0x50cd91b3u,
    0xd659e31du, 0x1d0530b8u, 0xec53826du, 0x270f51c8u, 0xa19b2366u, 0x6ac7f0c3u,
    0x77c2c07bu, 0xbc9e13deu, 0x3a0a6170u, 0xf156b2d5u, 0x03d6029bu, 0xc88ad13eu,
    0x4e1ea390u, 0x85427035u, 0x9847408du, 0x531b9328u, 0xd58fe186u, 0x1ed33223u,
    0xef8580f6u, 0x24d95353u, 0xa24d21fdu, 0x6911f258u, 0x7414c2e0u, 0xbf481145u,
    0x39dc63ebu, 0xf280b04eu, 0x07ac0536u, 0xccf0d693u, 0x4a64a43du, 0x81387798u,
    0x9c3d4720u, 0x57619485u, 0xd1f5e62bu, 0x1aa9358eu, 0xebff875bu, 0x20a354feu,
    0xa6372650u, 0x6d6bf5f5u, 0x706ec54du, 0xbb3216e8u, 0x3da66446u, 0xf6fab7e3u,
    0x047a07adu, 0xcf26d408u, 0x49b2a6a6u, 0x82ee7503u, 0x9feb45bbu, 0x54b7961eu,
    0xd223e4b0u, 0x197f3715u, 0xe82985c0u, 0x23755665u, 0xa5e124cbu, 0x6ebdf76eu,
    0x73b8c7d6u, 0xb8e41473u, 0x3e7066ddu, 0xf52cb578u, 0x0f580a6cu, 0xc404d9c9u,
    0x4290ab67u, 0x89cc78c2u, 0x94c9487au, 0x5f959bdfu, 0xd901e971u, 0x125d3ad4u,
    0xe30b8801u, 0x28575ba4u, 0xaec3290au, 0x659ffaafu, 0x789aca17u, 0xb3c619b2u,
    0x35526b1cu, 0xfe0eb8b9u, 0x0c8e08f7u, 0xc7d2db52u, 0x4146a9fcu, 0x8a1a7a59u,
    0x971f4ae1u, 0x5c439944u, 0xdad7ebeau, 0x118b384fu, 0xe0dd8a9au, 0x2b81593fu,
    0xad152b91u, 0x6649f834u, 0x7b4cc88cu, 0xb0101b29u, 0x36846987u, 0xfdd8ba22u,
    0x08f40f5au, 0xc3a8dcffu, 0x453cae51u, 0x8e607df4u, 0x93654d4cu, 0x58399ee9u,
    0xdeadec47u, 0x15f13fe2u, 0xe4a78d37u, 0x2ffb5e92u, 0xa96f2c3cu, 0x6233ff99u,
    0x7f36cf21u, 0xb46a1c84u, 0x32fe6e2au, 0xf9a2bd8fu, 0x0b220dc1u, 0xc07ede64u,
    0x46eaaccau, 0x8db67f6fu, 0x90b34fd7u, 0x5bef9c72u, 0xdd7beedcu, 0x16273d79u,
    0xe7718facu, 0x2c2d5c09u, 0xaab92ea7u, 0x61e5fd02u, 0x7ce0cdbau, 0xb7bc1e1fu,
    0x31286cb1u, 0xfa74bf14u, 0x1eb014d8u, 0xd5ecc77du, 0x5378b5d3u, 0x98246676u,
    0x852156ceu, 0x4e7d856bu, 0xc8e9f7c5u, 0x03b52460u, 0xf2e396b5u, 0x39bf4510u,
    0xbf2b37beu, 0x7477e41bu, 0x6972d4a3u, 0xa22e0706u, 0x24ba75a8u, 0xefe6a60du,
    0x1d661643u, 0xd63ac5e6u, 0x50aeb748u, 0x9bf264edu, 0x86f75455u, 0x4dab87f0u,
    0xcb3ff55eu, 0x006326fbu, 0xf135942eu, 0x3a69478bu, 0xbcfd3525u, 0x77a1e680u,
    0x6aa4d638u, 0xa1f8059du, 0x276c7733u, 0xec30a496u, 0x191c11eeu, 0xd240c24bu,
    0x54d4b0e5u, 0x9f886340u, 0x828d53f8u, 0x49d1805du, 0xcf45f2f3u, 0x04192156u,
    0xf54f9383u, 0x3e134026u, 0xb8873288u, 0x73dbe12du, 0x6eded195u, 0xa5820230u,
    0x2316709eu, 0xe84aa33bu, 0x1aca1375u, 0xd196c0d0u, 0x5702b27eu, 0x9c5e61dbu,
    0x815b5163u, 0x4a0782c6u, 0xcc93f068u, 0x07cf23cdu, 0xf6999118u, 0x3dc542bdu,
    0xbb513013u, 0x700de3b6u, 0x6d08d30eu, 0xa65400abu, 0x20c07205u, 0xeb9ca1a0u,
    0x11e81eb4u, 0xdab4cd11u, 0x5c20bfbfu, 0x977c6c1au, 0x8a795ca2u, 0x41258f07u,
    0xc7b1fda9u, 0x0ced2e0cu, 0xfdbb9cd9u, 0x36e74f7cu, 0xb0733dd2u, 0x7b2fee77u,
    0x662adecfu, 0xad760d6au, 0x2be27fc4u, 0xe0beac61u, 0x123e1c2fu, 0xd962cf8au,
    0x5ff6bd24u, 0x94aa6e81u, 0x89af5e39u, 0x42f38d9cu, 0xc467ff32u, 0x0f3b2c97u,
    0xfe6d9e42u, 0x35314de7u, 0xb3a53f49u, 0x78f9ececu, 0x65fcdc54u, 0xaea00ff1u,
    0x28347d5fu, 0xe368aefau, 0x16441b82u, 0xdd18c827u, 0x5b8cba89u, 0x90d0692cu,
    0x8dd55994u, 0x46898a31u, 0xc01df89fu, 0x0b412b3au, 0xfa1799efu, 0x314b4a4au,
    0xb7df38e4u, 0x7c83eb41u, 0x6186dbf9u, 0xaada085cu, 0x2c4e7af2u, 0xe712a957u,
    0x15921919u, 0xdececabcu, 0x585ab812u, 0x93066bb7u, 0x8e035b0fu, 0x455f88aau,
    0xc3cbfa04u, 0x089729a1u, 0xf9c19b74u, 0x329d48d1u, 0xb4093a7fu, 0x7f55e9dau,
    0x6250d962u, 0xa90c0ac7u, 0x2f987869u, 0xe4c4abccu
  },
  {
    0x00000000u, 0xa6770bb4u, 0x979f1129u, 0x31e81a9du, 0xf44f2413u, 0x52382fa7u,
    0x63d0353au, 0xc5a73e8eu, 0x33ef4e67u, 0x959845d3u, 0xa4705f4eu, 0x020754fau,
    0xc7a06a74u, 0x61d761c0u, 0x503f7b5du, 0xf64870e9u, 0x67de9cceu, 0xc1a9977au,
    0xf0418de7u, 0x56368653u, 0x9391b8ddu, 0x35e6b369u, 0x040ea9f4u, 0xa279a240u,
    0x5431d2a9u, 0xf246d91du, 0xc3aec380u, 0x65d9c834u, 0xa07ef6bau, 0x0609fd0eu,
    0x37e1e793u, 0x9196ec27u, 0xcfbd399cu, 0x69ca3228u, 0x582228b5u, 0xfe552301u,
    0x3bf21d8fu, 0x9d85163bu, 0xac6d0ca6u, 0x0a1a0712u, 0xfc5277fbu, 0x5a257c4fu,
    0x6bcd66d2u, 0xcdba6d66u, 0x081d53e8u, 0xae6a585cu, 0x9f8242c1u, 0x39f54975u,
    0xa863a552u, 0x0e14aee6u, 0x3ffcb47bu, 0x998bbfcfu, 0x5c2c8141u, 0xfa5b8af5u,
    0xcbb39068u, 0x6dc49bdcu, 0x9b8ceb35u, 0x3dfbe081u, 0x0c13fa1cu, 0xaa64f1a8u,
    0x6fc3cf26u, 0xc9b4c492u, 0xf85cde0fu, 0x5e2bd5bbu, 0x440b7579u, 0xe27c7ecdu,
    0xd3946450u, 0x75e36fe4u, 0xb044516au, 0x16335adeu, 0x27db4043u, 0x81ac4bf7u,
    0x77e43b1eu, 0xd19330aau, 0xe07b2a37u, 0x460c2183u, 0x83ab1f0du, 0x25dc14b9u,
    0x14340e24u, 0xb2430590u, 0x23d5e9b7u, 0x85a2e203u, 0xb44af89eu, 0x123df32au,
    0xd79acda4u, 0x71edc610u, 0x4005dc8du, 0xe672d739u, 0x103aa7d0u, 0xb64dac64u,
    0x87a5b6f9u, 0x21d2bd4du, 0xe47583c3u, 0x42028877u, 0x73ea92eau, 0xd59d995eu,
    0x8bb64ce5u, 0x2dc14751u, 0x1c295dccu, 0xba5e5678u, 0x7ff968f6u, 0xd98e6342u,
    0xe86679dfu, 0x4e11726bu, 0xb8590282u, 0x1e2e0936u, 0x2fc613abu, 0x89b1181fu,
    0x4c162691u, 0xea612d25u, 0xdb8937b8u, 0x7dfe3c0cu, 0xec68d02bu, 0x4a1fdb9fu,
    0x7bf7c102u, 0xdd80cab6u, 0x1827f438u, 0xbe50ff8cu, 0x8fb8e511u, 0x29cfeea5u,
    0xdf879e4cu, 0x79f095f8u, 0x48188f65u, 0xee6f84d1u, 0x2bc8ba5fu, 0x8dbfb1ebu,
    0xbc57ab76u, 0x1a20a0c2u, 0x8816eaf2u, 0x2e61e146u, 0x1f89fbdbu, 0xb9fef06fu,
    0x7c59cee1u, 0xda2ec555u, 0xebc6dfc8u, 0x4db1d47cu, 0xbbf9a495u, 0x1d8eaf21u,
    0x2c66b5bcu, 0x8a11be08u, 0x4fb68086u, 0xe9c18b32u, 0xd82991afu, 0x7e5e9a1bu,
    0xefc8763cu, 0x49bf7d88u, 0x78576715u, 0xde206ca1u, 0x1b87522fu, 0xbdf0599bu,
    0x8c184306u, 0x2a6f48b2u, 0xdc27385bu, 0x7a5033efu, 0x4bb82972u, 0xedcf22c6u,
    0x28681c48u, 0x8e1f17fcu, 0xbff70d61u, 0x198006d5u, 0x47abd36eu, 0xe1dcd8dau,
    0xd034c247u, 0x7643c9f3u, 0xb3e4f77du, 0x1593fcc9u, 0x247be654u, 0x820cede0u,
    0x74449d09u, 0xd23396bdu, 0xe3db8c20u, 0x45ac8794u, 0x800bb91au, 0x267cb2aeu,
    0x1794a833u, 0xb1e3a387u, 0x20754fa0u, 0x86024414u, 0xb7ea5e89u, 0x119d553du,
    0xd43a6bb3u, 0x724d6007u, 0x43a57a9au, 0xe5d2712eu, 0x139a01c7u, 0xb5ed0a73u,
    0x840510eeu, 0x22721b5au, 0xe7d525d4u, 0x41a22e60u, 0x704a34fdu, 0xd63d3f49u,
    0xcc1d9f8bu, 0x6a6a943fu, 0x5b828ea2u, 0xfdf58516u, 0x3852bb98u, 0x9e25b02cu,
    0xafcdaab1u, 0x09baa105u, 0xfff2d1ecu, 0x5985da58u, 0x686dc0c5u, 0xce1acb71u,
    0x0bbdf5ffu, 0xadcafe4bu, 0x9c22e4d6u, 0x3a55ef62u, 0xabc30345u, 0x0db408f1u,
    0x3c5c126cu, 0x9a2b19d8u, 0x5f8c2756u, 0xf9fb2ce2u, 0xc813367fu, 0x6e643dcbu,
    0x982c4d22u, 0x3e5b4696u, 0x0fb35c0bu, 0xa9c457bfu, 0x6c636931u, 0xca146285u,
    0xfbfc7818u, 0x5d8b73acu, 0x03a0a617u, 0xa5d7ada3u, 0x943fb73eu, 0x3248bc8au,
    0xf7ef8204u, 0x519889b0u, 0x6070932du, 0xc6079899u, 0x304fe870u, 0x9638e3c4u,
    0xa7d0f959u, 0x01a7f2edu, 0xc400cc63u, 0x6277c7d7u, 0x539fdd4au, 0xf5e8d6feu,
    0x647e3ad9u, 0xc209316du, 0xf3e12bf0u, 0x55962044u, 0x90311ecau, 0x3646157eu,
    0x07ae0fe3u, 0xa1d90457u, 0x579174beu, 0xf1e67f0au, 0xc00e6597u, 0x66796e23u,
    0xa3de50adu, 0x05a95b19u, 0x34414184u, 0x92364a30u
  },
  {
    0x00000000u, 0xccaa009eu, 0x4225077du, 0x8e8f07e3u, 0x844a0efau, 0x48e00e64u,
    0xc66f0987u, 0x0ac50919u, 0xd3e51bb5u, 0x1f4f1b2bu, 0x91c01cc8u, 0x5d6a1c56u,
    0x57af154fu, 0x9b0515d1u, 0x158a1232u, 0xd92012acu, 0x7cbb312bu, 0xb01131b5u,
    0x3e9e3656u, 0xf23436c8u, 0xf8f13fd1u, 0x345b3f4fu, 0xbad438acu, 0x767e3832u,
    0xaf5e2a9eu, 0x63f42a00u, 0xed7b2de3u, 0x21d12d7du, 0x2b142464u, 0xe7be24fau,
    0x69312319u, 0xa59b2387u, 0xf9766256u, 0x35dc62c8u, 0xbb53652bu, 0x77f965b5u,
    0x7d3c6cacu, 0xb1966c32u, 0x3f196bd1u, 0xf3b36b4fu, 0x2a9379e3u, 0xe639797du,
    0x68b67e9eu, 0xa41c7e00u, 0xaed97719u, 0x62737787u, 0xecfc7064u, 0x205670fau,
    0x85cd537du, 0x496753e3u, 0xc7e85400u, 0x0b42549eu, 0x01875d87u, 0xcd2d5d19u,
    0x43a25afau, 0x8f085a64u, 0x562848c8u, 0x9a824856u, 0x140d4fb5u, 0xd8a74f2bu,
    0xd2624632u, 0x1ec846acu, 0x9047414fu, 0x5ced41d1u, 0x299dc2edu, 0xe537c273u,
    0x6bb8c590u, 0xa712c50eu, 0xadd7cc17u, 0x617dcc89u, 0xeff2cb6au, 0x2358cbf4u,
    0xfa78d958u, 0x36d2d9c6u, 0xb85dde25u, 0x74f7debbu, 0x7e32d7a2u, 0xb298d73cu,
    0x3c17d0dfu, 0xf0bdd041u, 0x5526f3c6u, 0x998cf358u, 0x1703f4bbu, 0xdba9f425u,
    0xd16cfd3cu, 0x1dc6fda2u, 0x9349fa41u, 0x5fe3fadfu, 0x86c3e873u, 0x4a69e8edu,
    0xc4e6ef0eu, 0x084cef90u, 0x0289e689u, 0xce23e617u, 0x40ace1f4u, 0x8c06e16au,
    0xd0eba0bbu, 0x1c41a025u, 0x92cea7c6u, 0x5e64a758u, 0x54a1ae41u, 0x980baedfu,
    0x1684a93cu, 0xda2ea9a2u, 0x030ebb0eu, 0xcfa4bb90u, 0x412bbc73u, 0x8d81bcedu,
    0x8744b5f4u, 0x4beeb56au, 0xc561b289u, 0x09cbb217u, 0xac509190u, 0x60fa910eu,
    0xee7596edu, 0x22df9673u, 0x281a9f6au, 0xe4b09ff4u, 0x6a3f9817u, 0xa6959889u,
    0x7fb58a25u, 0xb31f8abbu, 0x3d908d58u, 0xf13a8dc6u, 0xfbff84dfu, 0x37558441u,
    0xb9da83a2u, 0x7570833cu, 0x533b85dau, 0x9f918544u, 0x111e82a7u, 0xddb48239u,
    0xd7718b20u, 0x1bdb8bbeu, 0x95548c5du, 0x59fe8cc3u, 0x80de9e6fu, 0x4c749ef1u,
    0xc2fb9912u, 0x0e51998cu, 0x04949095u, 0xc83e900bu, 0x46b197e8u, 0x8a1b9776u,
    0x2f80b4f1u, 0xe32ab46fu, 0x6da5b38cu, 0xa10fb312u, 0xabcaba0bu, 0x6760ba95u,
    0xe9efbd76u, 0x2545bde8u, 0xfc65af44u, 0x30cfafdau, 0xbe40a839u, 0x72eaa8a7u,
    0x782fa1beu, 0xb485a120u, 0x3a0aa6c3u, 0xf6a0a65du, 0xaa4de78cu, 0x66e7e712u,
    0xe868e0f1u, 0x24c2e06fu, 0x2e07e976u, 0xe2ade9e8u, 0x6c22ee0bu, 0xa088ee95u,
    0x79a8fc39u, 0xb502fca7u, 0x3b8dfb44u, 0xf727fbdau, 0xfde2f2c3u, 0x3148f25du,
    0xbfc7f5beu, 0x736df520u, 0xd6f6d6a7u, 0x1a5cd639u, 0x94d3d1dau, 0x5879d144u,
    0x52bcd85du, 0x9e16d8c3u, 0x1099df20u, 0xdc33dfbeu, 0x0513cd12u, 0xc9b9cd8cu,
    0x4736ca6fu, 0x8b9ccaf1u, 0x8159c3e8u, 0x4df3c376u, 0xc37cc495u, 0x0fd6c40bu,
    0x7aa64737u, 0xb60c47a9u, 0x3883404au, 0xf42940d4u, 0xfeec49cdu, 0x32464953u,
    0xbcc94eb0u, 0x70634e2eu, 0xa9435c82u, 0x65e95c1cu, 0xeb665bffu, 0x27cc5b61u,
    0x2d095278u, 0xe1a352e6u, 0x6f2c5505u, 0xa386559bu, 0x061d761cu, 0xcab77682u,
    0x44387161u, 0x889271ffu, 0x825778e6u, 0x4efd7878u, 0xc0727f9bu, 0x0cd87f05u,
    0xd5f86da9u, 0x19526d37u, 0x97dd6ad4u, 0x5b776a4au, 0x51b26353u, 0x9d1863cdu,
    0x1397642eu, 0xdf3d64b0u, 0x83d02561u, 0x4f7a25ffu, 0xc1f5221cu, 0x0d5f2282u,
    0x079a2b9bu, 0xcb302b05u, 0x45bf2ce6u, 0x89152c78u, 0x50353ed4u, 0x9c9f3e4au,
    0x121039a9u, 0xdeba3937u, 0xd47f302eu, 0x18d530b0u, 0x965a3753u, 0x5af037cdu,
    0xff6b144au, 0x33c114d4u, 0xbd4e1337u, 0x71e413a9u, 0x7b211ab0u, 0xb78b1a2eu,
    0x39041dcdu, 0xf5ae1d53u, 0x2c8e0fffu, 0xe0240f61u, 0x6eab0882u, 0xa201081cu,
    0xa8c40105u, 0x646e019bu, 0xeae10678u, 0x264b06e6u
  }
};

static uint32_t sp_zlib_crc32(uint32_t crc, const unsigned char *p, size_t n) {
  const uint32_t (*t)[256] = sp_zlib_crc_table;
  crc = ~crc;
  while (n >= 8) {
    /* The two words are put together a byte at a time, low byte first: the
       same value on either byte order, and a plain load where the order
       is already that one. */
    uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    crc = t[7][a & 0xff] ^ t[6][(a >> 8) & 0xff] ^ t[5][(a >> 16) & 0xff] ^ t[4][a >> 24] ^
          t[3][b & 0xff] ^ t[2][(b >> 8) & 0xff] ^ t[1][(b >> 16) & 0xff] ^ t[0][b >> 24];
    p += 8;
    n -= 8;
  }
  while (n--) crc = t[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

static uint32_t sp_zlib_adler32(uint32_t adler, const unsigned char *p, size_t n) {
  uint32_t a = adler & 0xffff, b = (adler >> 16) & 0xffff;
  /* 5552 is the largest run of bytes that cannot overflow b before the
     modulo, which is why the loop is chunked rather than reducing per byte. */
  while (n) {
    size_t k = n < 5552 ? n : 5552;
    n -= k;
    while (k--) { a += *p++; b += a; }
    a %= 65521; b %= 65521;
  }
  return (b << 16) | a;
}

/* -------------------------------------------------------------- output sink */

typedef struct { unsigned char *p; size_t len, cap; int err; } sp_zlib_out;

static int sp_zlib_out_reserve(sp_zlib_out *o, size_t extra) {
  if (o->err) return 0;
  if (o->len + extra <= o->cap) return 1;
  size_t cap = o->cap ? o->cap : 4096;
  while (cap < o->len + extra) {
    if (cap > (SIZE_MAX >> 8)) { o->err = 1; return 0; }   /* refuse to spiral (`1 << 40` is 0 in a 32-bit size_t) */
    cap *= 2;
  }
  unsigned char *np = (unsigned char *)realloc(o->p, cap);
  if (!np) { o->err = 1; return 0; }
  o->p = np; o->cap = cap;
  return 1;
}

static void sp_zlib_out_byte(sp_zlib_out *o, unsigned char c) {
  if (!sp_zlib_out_reserve(o, 1)) return;
  o->p[o->len++] = c;
}

static void sp_zlib_out_write(sp_zlib_out *o, const unsigned char *src, size_t n) {
  if (!sp_zlib_out_reserve(o, n)) return;
  memcpy(o->p + o->len, src, n);
  o->len += n;
}

/* An LZ77 back-reference may overlap its own output (`dist` 1 repeats one
   byte `len` times), so this copies forward one byte at a time rather than
   memcpy'ing a region that is still being written. */
static void sp_zlib_out_repeat(sp_zlib_out *o, size_t dist, size_t len) {
  if (dist == 0 || dist > o->len) { o->err = 1; return; }
  if (!sp_zlib_out_reserve(o, len)) return;
  size_t from = o->len - dist;
  for (size_t i = 0; i < len; i++) o->p[o->len + i] = o->p[from + i];
  o->len += len;
}

/* ------------------------------------------------------------- bit reader */

typedef struct {
  const unsigned char *src;
  size_t len, pos;
  uint32_t bitbuf;
  int bitcnt;
  int err;
  int eof;
} sp_zlib_in;

/* DEFLATE packs bits low-order first within each byte. -1 means the stream
   ended mid-code, which is a truncated input rather than a malformed one:
   `eof` is set for it, and only an inflate in pieces tells them apart. */
static int sp_zlib_in_bits(sp_zlib_in *s, int need) {
  while (s->bitcnt < need) {
    if (s->pos >= s->len) { s->err = 1; s->eof = 1; return -1; }
    s->bitbuf |= (uint32_t)s->src[s->pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  int v = (int)(s->bitbuf & ((1u << need) - 1));
  s->bitbuf >>= need;
  s->bitcnt -= need;
  return v;
}

/* ------------------------------------------------------------ Huffman codes */

/* Canonical Huffman, decoded by walking code lengths: `counts[l]` is how many
   symbols have length l, `symbols` lists them in that order. Decoding one
   symbol walks lengths 1..15 accumulating bits, which is small and needs no
   table build beyond a prefix sum. */
typedef struct { int counts[16]; int symbols[288]; } sp_zlib_huff;

static int sp_zlib_huff_build(sp_zlib_huff *h, const unsigned char *lengths, int n) {
  for (int i = 0; i < 16; i++) h->counts[i] = 0;
  for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
  if (h->counts[0] == n) return 0;   /* no codes at all: caller decides */
  /* Reject an over-subscribed set: more codes at some length than the tree
     can hold. An incomplete set is legal (a single distance code is), so only
     the over-subscribed direction is an error. */
  int left = 1;
  for (int l = 1; l < 16; l++) {
    left <<= 1;
    left -= h->counts[l];
    if (left < 0) return -1;
  }
  int offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->counts[l];
  for (int i = 0; i < n; i++)
    if (lengths[i]) h->symbols[offs[lengths[i]]++] = i;
  return 0;
}

static int sp_zlib_huff_decode(sp_zlib_in *s, const sp_zlib_huff *h) {
  int code = 0, first = 0, index = 0;
  for (int l = 1; l < 16; l++) {
    int b = sp_zlib_in_bits(s, 1);
    if (b < 0) return -1;
    code |= b;
    int count = h->counts[l];
    if (code - first < count) return h->symbols[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->err = 1;
  return -1;
}

/* ------------------------------------------------------------ length tables */

static const unsigned short SP_Z_LEN_BASE[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
  67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char SP_Z_LEN_EXTRA[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
  4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short SP_Z_DIST_BASE[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
  769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char SP_Z_DIST_EXTRA[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
  9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* --------------------------------------------------------------- inflate */

static int sp_zlib_block_stored(sp_zlib_in *s, sp_zlib_out *o) {
  s->bitbuf = 0; s->bitcnt = 0;              /* stored blocks start byte-aligned */
  if (s->pos + 4 > s->len) { s->err = 1; return -1; }
  unsigned len  = (unsigned)s->src[s->pos] | ((unsigned)s->src[s->pos + 1] << 8);
  unsigned nlen = (unsigned)s->src[s->pos + 2] | ((unsigned)s->src[s->pos + 3] << 8);
  s->pos += 4;
  if ((len ^ 0xffff) != nlen) { s->err = 1; return -1; }
  if (s->pos + len > s->len) { s->err = 1; return -1; }
  sp_zlib_out_write(o, s->src + s->pos, len);
  s->pos += len;
  return o->err ? -1 : 0;
}

/* One literal, or one match with its length and distance: 1 at the end of
   the block. Nothing is written for a match the input ends in the middle
   of. Inlined into both of its calls. */
static SP_INLINE int sp_zlib_block_code(sp_zlib_in *s, sp_zlib_out *o,
                                        const sp_zlib_huff *lit, const sp_zlib_huff *dist) {
  int sym = sp_zlib_huff_decode(s, lit);
  if (sym < 0) return -1;
  if (sym < 256) { sp_zlib_out_byte(o, (unsigned char)sym); return o->err ? -1 : 0; }
  if (sym == 256) return 1;                       /* end of block */
  sym -= 257;
  if (sym >= 29) { s->err = 1; return -1; }
  int extra = sp_zlib_in_bits(s, SP_Z_LEN_EXTRA[sym]);
  if (extra < 0) return -1;
  size_t len = (size_t)SP_Z_LEN_BASE[sym] + (size_t)extra;
  int dsym = sp_zlib_huff_decode(s, dist);
  if (dsym < 0) return -1;
  if (dsym >= 30) { s->err = 1; return -1; }
  int dextra = sp_zlib_in_bits(s, SP_Z_DIST_EXTRA[dsym]);
  if (dextra < 0) return -1;
  size_t d = (size_t)SP_Z_DIST_BASE[dsym] + (size_t)dextra;
  if (d > o->len) { s->err = 1; return -1; }      /* before the start of the output */
  sp_zlib_out_repeat(o, d, len);
  return o->err ? -1 : 0;
}

static int sp_zlib_block_codes(sp_zlib_in *s, sp_zlib_out *o,
                            const sp_zlib_huff *lit, const sp_zlib_huff *dist) {
  for (;;) {
    int rc = sp_zlib_block_code(s, o, lit, dist);
    if (rc) return rc < 0 ? -1 : 0;
  }
}

static void sp_zlib_fixed_tables(sp_zlib_huff *lit, sp_zlib_huff *dist) {
  unsigned char l[288], d[30];
  int i = 0;
  for (; i < 144; i++) l[i] = 8;
  for (; i < 256; i++) l[i] = 9;
  for (; i < 280; i++) l[i] = 7;
  for (; i < 288; i++) l[i] = 8;
  for (i = 0; i < 30; i++) d[i] = 5;
  sp_zlib_huff_build(lit, l, 288);
  sp_zlib_huff_build(dist, d, 30);
}

/* The order the dynamic-block header lists code-length code lengths in. */
static const unsigned char SP_Z_CLEN_ORDER[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* The two codes of a dynamic block, read from its header. */
static int sp_zlib_dynamic_tables(sp_zlib_in *s, sp_zlib_huff *lit, sp_zlib_huff *dist) {
  int hlit  = sp_zlib_in_bits(s, 5); if (hlit  < 0) return -1;
  int hdist = sp_zlib_in_bits(s, 5); if (hdist < 0) return -1;
  int hclen = sp_zlib_in_bits(s, 4); if (hclen < 0) return -1;
  int nlen = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;
  if (nlen > 286 || ndist > 30) { s->err = 1; return -1; }

  unsigned char clens[19];
  memset(clens, 0, sizeof clens);
  for (int i = 0; i < ncode; i++) {
    int v = sp_zlib_in_bits(s, 3);
    if (v < 0) return -1;
    clens[SP_Z_CLEN_ORDER[i]] = (unsigned char)v;
  }
  sp_zlib_huff clh;
  if (sp_zlib_huff_build(&clh, clens, 19) < 0) { s->err = 1; return -1; }

  unsigned char lengths[288 + 30];
  memset(lengths, 0, sizeof lengths);
  int n = 0;
  while (n < nlen + ndist) {
    int sym = sp_zlib_huff_decode(s, &clh);
    if (sym < 0) return -1;
    if (sym < 16) { lengths[n++] = (unsigned char)sym; continue; }
    int rep = 0;
    unsigned char val = 0;
    if (sym == 16) {
      if (n == 0) { s->err = 1; return -1; }   /* nothing to repeat */
      val = lengths[n - 1];
      int e = sp_zlib_in_bits(s, 2); if (e < 0) return -1;
      rep = 3 + e;
    }
    else if (sym == 17) { int e = sp_zlib_in_bits(s, 3); if (e < 0) return -1; rep = 3 + e; }
    else                { int e = sp_zlib_in_bits(s, 7); if (e < 0) return -1; rep = 11 + e; }
    if (n + rep > nlen + ndist) { s->err = 1; return -1; }
    while (rep--) lengths[n++] = val;
  }
  if (lengths[256] == 0) { s->err = 1; return -1; }   /* no end-of-block code */

  if (sp_zlib_huff_build(lit, lengths, nlen) < 0) { s->err = 1; return -1; }
  if (sp_zlib_huff_build(dist, lengths + nlen, ndist) < 0) { s->err = 1; return -1; }
  return 0;
}

static int sp_zlib_block_dynamic(sp_zlib_in *s, sp_zlib_out *o) {
  sp_zlib_huff lit, dist;
  if (sp_zlib_dynamic_tables(s, &lit, &dist) < 0) return -1;
  return sp_zlib_block_codes(s, o, &lit, &dist);
}

static int sp_zlib_inflate_raw(sp_zlib_in *s, sp_zlib_out *o) {
  for (;;) {
    int final = sp_zlib_in_bits(s, 1); if (final < 0) return -1;
    int type  = sp_zlib_in_bits(s, 2); if (type  < 0) return -1;
    int rc;
    if (type == 0) rc = sp_zlib_block_stored(s, o);
    else if (type == 1) {
      sp_zlib_huff lit, dist;
      sp_zlib_fixed_tables(&lit, &dist);
      rc = sp_zlib_block_codes(s, o, &lit, &dist);
    }
    else if (type == 2) rc = sp_zlib_block_dynamic(s, o);
    else { s->err = 1; return -1; }
    if (rc < 0) return -1;
    if (final) return 0;
  }
}

/* ------------------------------------------------------------- the wrappers */

/* window_bits, spelled as zlib spells it: 8..15 a zlib stream, negative raw
   deflate, +16 gzip, +32 accept either. */
#define SP_Z_RAW  0
#define SP_Z_ZLIB 1
#define SP_Z_GZIP 2
#define SP_Z_AUTO 3

static int sp_zlib_wrap_kind(int window_bits) {
  if (window_bits < 0) return SP_Z_RAW;
  if (window_bits >= 40) return SP_Z_AUTO;
  if (window_bits >= 24) return SP_Z_GZIP;
  return SP_Z_ZLIB;
}

static SP_TLS const char *sp_zlib_err = NULL;

const char *sp_zlib_last_error(void) { return sp_zlib_err ? sp_zlib_err : ""; }

/* Skip a gzip header, answering the offset of the deflate data or -1. */
static long sp_zlib_gzip_header(const unsigned char *p, size_t n) {
  if (n < 10 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return -1;
  unsigned flg = p[3];
  size_t i = 10;
  if (flg & 4) {                                    /* FEXTRA */
    if (i + 2 > n) return -1;
    size_t xlen = (size_t)p[i] | ((size_t)p[i + 1] << 8);
    i += 2 + xlen;
    if (i > n) return -1;
  }
  if (flg & 8)  { while (i < n && p[i]) i++; if (i >= n) return -1; i++; }   /* FNAME */
  if (flg & 16) { while (i < n && p[i]) i++; if (i >= n) return -1; i++; }   /* FCOMMENT */
  if (flg & 2)  { i += 2; if (i > n) return -1; }   /* FHCRC */
  return (long)i;
}

/* Zlib.inflate / Zlib::Inflate#inflate. Returns NULL on a malformed stream;
   the Ruby side turns that into Zlib::DataError with sp_zlib_last_error. */
const char *sp_zlib_inflate(const char *src, sp_int window_bits) {SP_GC_ROOT_STR(src);
  sp_zlib_err = NULL;
  size_t n = src ? sp_str_byte_len(src) : 0;
  const unsigned char *p = (const unsigned char *)src;
  int kind = sp_zlib_wrap_kind((int)window_bits);
  size_t off = 0;
  uint32_t want = 0;
  int check = 0;   /* 0 none, 1 adler32, 2 crc32 */

  if (kind == SP_Z_AUTO) {
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) kind = SP_Z_GZIP;
    else kind = SP_Z_ZLIB;
  }
  if (kind == SP_Z_ZLIB) {
    /* CMF/FLG: low nibble of CMF must be 8 (deflate) and the pair must be a
       multiple of 31, which is the whole of the zlib header's self-check. */
    if (n < 2 || (p[0] & 0x0f) != 8 || ((p[0] << 8) | p[1]) % 31 != 0) {
      sp_zlib_err = "incorrect header check";
      return NULL;
    }
    if (p[1] & 0x20) { sp_zlib_err = "preset dictionary not supported"; return NULL; }
    off = 2;
    if (n >= 6) {
      want = ((uint32_t)p[n - 4] << 24) | ((uint32_t)p[n - 3] << 16) |
             ((uint32_t)p[n - 2] << 8) | (uint32_t)p[n - 1];
      check = 1;
    }
  }
  else if (kind == SP_Z_GZIP) {
    long h = sp_zlib_gzip_header(p, n);
    if (h < 0) { sp_zlib_err = "not in gzip format"; return NULL; }
    off = (size_t)h;
    if (n >= off + 8) {
      want = (uint32_t)p[n - 8] | ((uint32_t)p[n - 7] << 8) |
             ((uint32_t)p[n - 6] << 16) | ((uint32_t)p[n - 5] << 24);
      check = 2;
    }
  }

  sp_zlib_in s;
  s.src = p + off; s.len = n - off; s.pos = 0;
  s.bitbuf = 0; s.bitcnt = 0; s.err = 0; s.eof = 0;
  sp_zlib_out o;
  o.p = NULL; o.len = 0; o.cap = 0; o.err = 0;

  if (sp_zlib_inflate_raw(&s, &o) < 0) {
    free(o.p);
    sp_zlib_err = o.err ? "out of memory" : "invalid or incomplete deflate data";
    return NULL;
  }
  if (check == 1 && sp_zlib_adler32(1, o.p, o.len) != want) {
    free(o.p); sp_zlib_err = "invalid stored block lengths"; return NULL;
  }
  if (check == 2 && sp_zlib_crc32(0, o.p, o.len) != want) {
    free(o.p); sp_zlib_err = "invalid compressed data -- crc error"; return NULL;
  }

  char *r = sp_str_alloc_raw(o.len + 1);
  if (o.len) memcpy(r, o.p, o.len);
  r[o.len] = 0;
  sp_str_set_len(r, o.len);
  sp_str_mark_binary(r);
  free(o.p);
  return r;
}

/* --------------------------------------------------------------- deflate */

/* Bit writer, low-order first, matching the reader above. */
typedef struct { sp_zlib_out *o; uint64_t buf; int cnt; } sp_zlib_bw;

/* Appends value as nbits bits, 32 at most; value has no bit set above
   them. Fewer than 32 bits wait in the buffer between calls, so a whole
   match fits in one call, and whole bytes leave four at a time behind one
   reserve. */
static void sp_zlib_bw_put(sp_zlib_bw *w, uint64_t value, int nbits) {
  w->buf |= value << w->cnt;
  w->cnt += nbits;
  if (w->cnt >= 32) {
    sp_zlib_out *o = w->o;
    if (sp_zlib_out_reserve(o, 4)) {
      o->p[o->len]     = (unsigned char)w->buf;
      o->p[o->len + 1] = (unsigned char)(w->buf >> 8);
      o->p[o->len + 2] = (unsigned char)(w->buf >> 16);
      o->p[o->len + 3] = (unsigned char)(w->buf >> 24);
      o->len += 4;
    }
    w->buf >>= 32;
    w->cnt -= 32;
  }
}

/* Writes out the bits still waiting, the last byte padded with zeros. */
static void sp_zlib_bw_flush(sp_zlib_bw *w) {
  while (w->cnt > 0) {
    sp_zlib_out_byte(w->o, (unsigned char)(w->buf & 0xff));
    w->buf >>= 8;
    w->cnt -= 8;
  }
  w->buf = 0; w->cnt = 0;
}

/* The fixed literal/length code of RFC 1951 3.2.6, one entry per symbol:
   the length of the code in the top four bits, and below them the code
   with its bits in reverse order, because Huffman codes go out
   most-significant bit first, the one place DEFLATE reverses its own bit
   order.
     symbols   0-143: 8 bits, codes 0x30-0xbf
     symbols 144-255: 9 bits, codes 0x190-0x1ff
     symbols 256-279: 7 bits, codes 0x00-0x17
     symbols 280-287: 8 bits, codes 0xc0-0xc7
   Constants, not filled in on first use, for the reason the CRC rows give. */
static const unsigned short sp_zlib_fixed_lit_code[288] = {
  0x800c, 0x808c, 0x804c, 0x80cc, 0x802c, 0x80ac, 0x806c, 0x80ec, 0x801c, 0x809c,
  0x805c, 0x80dc, 0x803c, 0x80bc, 0x807c, 0x80fc, 0x8002, 0x8082, 0x8042, 0x80c2,
  0x8022, 0x80a2, 0x8062, 0x80e2, 0x8012, 0x8092, 0x8052, 0x80d2, 0x8032, 0x80b2,
  0x8072, 0x80f2, 0x800a, 0x808a, 0x804a, 0x80ca, 0x802a, 0x80aa, 0x806a, 0x80ea,
  0x801a, 0x809a, 0x805a, 0x80da, 0x803a, 0x80ba, 0x807a, 0x80fa, 0x8006, 0x8086,
  0x8046, 0x80c6, 0x8026, 0x80a6, 0x8066, 0x80e6, 0x8016, 0x8096, 0x8056, 0x80d6,
  0x8036, 0x80b6, 0x8076, 0x80f6, 0x800e, 0x808e, 0x804e, 0x80ce, 0x802e, 0x80ae,
  0x806e, 0x80ee, 0x801e, 0x809e, 0x805e, 0x80de, 0x803e, 0x80be, 0x807e, 0x80fe,
  0x8001, 0x8081, 0x8041, 0x80c1, 0x8021, 0x80a1, 0x8061, 0x80e1, 0x8011, 0x8091,
  0x8051, 0x80d1, 0x8031, 0x80b1, 0x8071, 0x80f1, 0x8009, 0x8089, 0x8049, 0x80c9,
  0x8029, 0x80a9, 0x8069, 0x80e9, 0x8019, 0x8099, 0x8059, 0x80d9, 0x8039, 0x80b9,
  0x8079, 0x80f9, 0x8005, 0x8085, 0x8045, 0x80c5, 0x8025, 0x80a5, 0x8065, 0x80e5,
  0x8015, 0x8095, 0x8055, 0x80d5, 0x8035, 0x80b5, 0x8075, 0x80f5, 0x800d, 0x808d,
  0x804d, 0x80cd, 0x802d, 0x80ad, 0x806d, 0x80ed, 0x801d, 0x809d, 0x805d, 0x80dd,
  0x803d, 0x80bd, 0x807d, 0x80fd, 0x9013, 0x9113, 0x9093, 0x9193, 0x9053, 0x9153,
  0x90d3, 0x91d3, 0x9033, 0x9133, 0x90b3, 0x91b3, 0x9073, 0x9173, 0x90f3, 0x91f3,
  0x900b, 0x910b, 0x908b, 0x918b, 0x904b, 0x914b, 0x90cb, 0x91cb, 0x902b, 0x912b,
  0x90ab, 0x91ab, 0x906b, 0x916b, 0x90eb, 0x91eb, 0x901b, 0x911b, 0x909b, 0x919b,
  0x905b, 0x915b, 0x90db, 0x91db, 0x903b, 0x913b, 0x90bb, 0x91bb, 0x907b, 0x917b,
  0x90fb, 0x91fb, 0x9007, 0x9107, 0x9087, 0x9187, 0x9047, 0x9147, 0x90c7, 0x91c7,
  0x9027, 0x9127, 0x90a7, 0x91a7, 0x9067, 0x9167, 0x90e7, 0x91e7, 0x9017, 0x9117,
  0x9097, 0x9197, 0x9057, 0x9157, 0x90d7, 0x91d7, 0x9037, 0x9137, 0x90b7, 0x91b7,
  0x9077, 0x9177, 0x90f7, 0x91f7, 0x900f, 0x910f, 0x908f, 0x918f, 0x904f, 0x914f,
  0x90cf, 0x91cf, 0x902f, 0x912f, 0x90af, 0x91af, 0x906f, 0x916f, 0x90ef, 0x91ef,
  0x901f, 0x911f, 0x909f, 0x919f, 0x905f, 0x915f, 0x90df, 0x91df, 0x903f, 0x913f,
  0x90bf, 0x91bf, 0x907f, 0x917f, 0x90ff, 0x91ff, 0x7000, 0x7040, 0x7020, 0x7060,
  0x7010, 0x7050, 0x7030, 0x7070, 0x7008, 0x7048, 0x7028, 0x7068, 0x7018, 0x7058,
  0x7038, 0x7078, 0x7004, 0x7044, 0x7024, 0x7064, 0x7014, 0x7054, 0x7034, 0x7074,
  0x8003, 0x8083, 0x8043, 0x80c3, 0x8023, 0x80a3, 0x8063, 0x80e3
};

/* The fixed distance code: each of the 30 symbols is its own number in
   five bits, here with the bits reversed. */
static const unsigned char sp_zlib_fixed_dist_code[30] = {
   0, 16,  8, 24,  4, 20, 12, 28,  2, 18, 10, 26,  6, 22, 14,
  30,  1, 17,  9, 25,  5, 21, 13, 29,  3, 19, 11, 27,  7, 23
};

/* A literal byte, or the end of the block, in the fixed code. */
static void sp_zlib_emit_lit(sp_zlib_bw *w, unsigned sym) {
  unsigned c = sp_zlib_fixed_lit_code[sym];
  sp_zlib_bw_put(w, c & 0xfff, (int)(c >> 12));
}

/* The length symbol (0-28) of a match length of 3-258. Lengths 3-10 have
   a symbol each; after them every four symbols cover twice the lengths of
   the four before, so the top bit of len - 3 picks the group and the two
   bits under it the symbol. 258 has a symbol of its own. */
static int sp_zlib_len_sym(size_t len) {
  uint64_t x = (uint64_t)len - 3;
  if (x < 8) return (int)x;
  if (len == 258) return 28;
  int top = 63 - SP_CLZ64(x);
  return 4 * (top - 1) + (int)((x >> (top - 2)) & 3);
}

/* The distance symbol (0-29) of a distance of 1-32768. Distances 1-4 have
   a symbol each; after them every two symbols cover twice the distances of
   the two before, so the top bit of d - 1 picks the pair and the bit under
   it the symbol. */
static int sp_zlib_dist_sym(size_t d) {
  uint64_t x = (uint64_t)d - 1;
  if (x < 4) return (int)x;
  int top = 63 - SP_CLZ64(x);
  return 2 * top + (int)((x >> (top - 1)) & 1);
}

/* One match as one call to the writer: the length code, its extra bits,
   the distance code and its extra bits, 31 bits at most. A symbol without
   extra bits has a base equal to the value, so its extra part is zero. */
static void sp_zlib_emit_match(sp_zlib_bw *w, size_t len, size_t dist) {
  int ls = sp_zlib_len_sym(len), ds = sp_zlib_dist_sym(dist);
  unsigned c = sp_zlib_fixed_lit_code[257 + ls];
  int nbits = (int)(c >> 12);
  uint64_t bits = c & 0xfff;
  bits |= (uint64_t)(len - SP_Z_LEN_BASE[ls]) << nbits;
  nbits += SP_Z_LEN_EXTRA[ls];
  bits |= (uint64_t)sp_zlib_fixed_dist_code[ds] << nbits;
  nbits += 5;
  bits |= (uint64_t)(dist - SP_Z_DIST_BASE[ds]) << nbits;
  nbits += SP_Z_DIST_EXTRA[ds];
  sp_zlib_bw_put(w, bits, nbits);
}

#define SP_Z_HASH_BITS 15
#define SP_Z_HASH_SIZE (1 << SP_Z_HASH_BITS)
#define SP_Z_MIN_MATCH 3
#define SP_Z_MAX_MATCH 258
#define SP_Z_WINDOW    32768
/* The length of the chain table for a long input. Twice the window: the
   entry a position takes over is then never one the search can still
   reach, whichever of search and insert comes first, and the index is the
   low 16 bits of the position. */
#define SP_Z_RING      (2 * SP_Z_WINDOW)

static unsigned sp_zlib_hash3(const unsigned char *p) {
  return (unsigned)(((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
         & (SP_Z_HASH_SIZE - 1);
}

/* Greedy LZ77 over a hash chain, emitted as one fixed-Huffman block. The
   chain depth is what `level` buys: more candidates examined per position,
   the same output format either way.

   `prev` is a ring of SP_Z_RING entries, not an entry per input byte, and
   `mask` turns a position into its entry. The search follows a chain only
   from a position at most SP_Z_WINDOW behind the one it is at, and the
   position that takes over that entry is SP_Z_RING further on, which the
   search has not reached: an entry is never read once it has been
   overwritten. An input that fits the ring has an entry per byte as it
   always had, and its mask is all ones. The function is inlined into each
   of its two calls so that this mask folds away there, and such an input is
   searched by the code that searched it before. */
static SP_INLINE void sp_zlib_deflate_body(sp_zlib_bw *w, const unsigned char *p, size_t n, int level, size_t mask) {
  /* Z_DEFAULT_COMPRESSION is -1 and means level 6, which is the value nearly
     every caller passes without knowing it: `Zlib.deflate(s)` takes the
     default, and reading -1 as "below 0, so no matching" made the default the
     one level that does not compress. */
  if (level < 0) level = 6;
  int depth = level == 0 ? 0 : (level <= 3 ? 16 : (level <= 6 ? 64 : 256));
  int32_t *head = NULL, *prev = NULL;
  if (depth > 0 && n >= SP_Z_MIN_MATCH) {
    size_t ring = n < SP_Z_RING ? n : SP_Z_RING;
    head = (int32_t *)malloc(sizeof(int32_t) * SP_Z_HASH_SIZE);
    prev = (int32_t *)malloc(sizeof(int32_t) * ring);
    if (!head || !prev) { free(head); free(prev); head = NULL; prev = NULL; depth = 0; }
  }
  if (head) for (size_t i = 0; i < SP_Z_HASH_SIZE; i++) head[i] = -1;

  size_t i = 0;
  while (i < n) {
    size_t best_len = 0, best_dist = 0;
    if (head && i + SP_Z_MIN_MATCH <= n) {
      unsigned h = sp_zlib_hash3(p + i);
      int32_t cand = head[h];
      int tries = depth;
      size_t limit = n - i;
      if (limit > SP_Z_MAX_MATCH) limit = SP_Z_MAX_MATCH;
      while (cand >= 0 && tries-- > 0) {
        size_t d = i - (size_t)cand;
        if (d > SP_Z_WINDOW) break;              /* chains are newest first */
        const unsigned char *a = p + (size_t)cand, *b = p + i;
        /* A candidate replaces the best match only by being longer, so it
           has to agree with the input one byte past the best match: at the
           very first byte when there is none yet. That byte is inside the
           input, since a match of `limit` bytes ends the search. */
        if (a[best_len] != b[best_len]) { cand = prev[cand & mask]; continue; }
        size_t l = 0;
        while (l + 8 <= limit && memcmp(a + l, b + l, 8) == 0) l += 8;
        while (l < limit && a[l] == b[l]) l++;
        if (l > best_len) { best_len = l; best_dist = d; if (l == limit) break; }
        cand = prev[cand & mask];
      }
      if (best_len < SP_Z_MIN_MATCH) { best_len = 0; best_dist = 0; }
    }

    size_t advance = best_len ? best_len : 1;
    if (best_len) sp_zlib_emit_match(w, best_len, best_dist);
    else          sp_zlib_emit_lit(w, p[i]);
    /* Every position the match covered still enters the chain: skipping them
       is what makes a later match miss the only copy it had. */
    if (head)
      for (size_t k = i; k < i + advance && k + SP_Z_MIN_MATCH <= n; k++) {
        unsigned h = sp_zlib_hash3(p + k);
        prev[k & mask] = head[h];
        head[h] = (int32_t)k;
      }
    i += advance;
  }
  sp_zlib_emit_lit(w, 256);   /* end of block */
  free(head);
  free(prev);
}

/* `n` bytes at `p` as one fixed-Huffman block, BFINAL set when `final`.
   The bits of the block's end wait in `w`. */
static void sp_zlib_deflate_block_into(sp_zlib_bw *w, const unsigned char *p, size_t n, int level, int final) {
  sp_zlib_bw_put(w, final ? 1 : 0, 1);   /* BFINAL */
  sp_zlib_bw_put(w, 1, 2);               /* BTYPE 01: fixed Huffman */
  /* two calls, two inlined bodies: the mask is a constant in each */
  if (n <= SP_Z_RING) sp_zlib_deflate_body(w, p, n, level, ~(size_t)0);
  else                sp_zlib_deflate_body(w, p, n, level, SP_Z_RING - 1);
}

/* The output as a length-set BINARY string on the GC heap, its buffer
   freed; NULL, with the error message set, when it could not be grown. */
static const char *sp_zlib_out_str(sp_zlib_out *o) {
  if (o->err) { free(o->p); sp_zlib_err = "out of memory"; return NULL; }
  char *r = sp_str_alloc_raw(o->len + 1);
  if (o->len) memcpy(r, o->p, o->len);
  r[o->len] = 0;
  sp_str_set_len(r, o->len);
  sp_str_mark_binary(r);
  free(o->p);
  return r;
}

/* The whole of `src` as one fixed-Huffman block, in the wrapper that
   `window_bits` names: a zlib stream, a gzip member, or raw deflate. NULL,
   with the error message set, when the output cannot be allocated. */
const char *sp_zlib_deflate(const char *src, sp_int level, sp_int window_bits) {SP_GC_ROOT_STR(src);
  sp_zlib_err = NULL;
  size_t n = src ? sp_str_byte_len(src) : 0;
  const unsigned char *p = (const unsigned char *)src;
  int kind = sp_zlib_wrap_kind((int)window_bits);
  if (kind == SP_Z_AUTO) kind = SP_Z_ZLIB;   /* nothing to detect when writing */

  sp_zlib_out o;
  o.p = NULL; o.len = 0; o.cap = 0; o.err = 0;

  if (kind == SP_Z_ZLIB) {
    /* CMF 0x78: deflate, 32K window. FLG chosen so the pair divides by 31. */
    sp_zlib_out_byte(&o, 0x78);
    sp_zlib_out_byte(&o, 0x9c);
  }
  else if (kind == SP_Z_GZIP) {
    static const unsigned char hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
    sp_zlib_out_write(&o, hdr, 10);
  }

  sp_zlib_bw w;
  w.o = &o; w.buf = 0; w.cnt = 0;
  sp_zlib_deflate_block_into(&w, p, n, (int)level, 1);   /* one block for the whole input */
  sp_zlib_bw_flush(&w);

  if (kind == SP_Z_ZLIB) {
    uint32_t a = sp_zlib_adler32(1, p, n);
    sp_zlib_out_byte(&o, (unsigned char)(a >> 24));
    sp_zlib_out_byte(&o, (unsigned char)(a >> 16));
    sp_zlib_out_byte(&o, (unsigned char)(a >> 8));
    sp_zlib_out_byte(&o, (unsigned char)a);
  }
  else if (kind == SP_Z_GZIP) {
    uint32_t c = sp_zlib_crc32(0, p, n);
    sp_zlib_out_byte(&o, (unsigned char)c);
    sp_zlib_out_byte(&o, (unsigned char)(c >> 8));
    sp_zlib_out_byte(&o, (unsigned char)(c >> 16));
    sp_zlib_out_byte(&o, (unsigned char)(c >> 24));
    uint32_t sz = (uint32_t)n;
    sp_zlib_out_byte(&o, (unsigned char)sz);
    sp_zlib_out_byte(&o, (unsigned char)(sz >> 8));
    sp_zlib_out_byte(&o, (unsigned char)(sz >> 16));
    sp_zlib_out_byte(&o, (unsigned char)(sz >> 24));
  }

  return sp_zlib_out_str(&o);
}

/* `src` as one raw fixed-Huffman block, for a stream written in pieces
   (Zlib::Deflate#deflate with a flush): no wrapper, which the Ruby side
   writes around the blocks with the checksum it keeps. The last block has
   BFINAL set. Any other ends on a byte, after the empty stored block that
   zlib's sync flush writes (00 00 ff ff), so an inflater has all of it at
   once. A block's matches stay inside it: each starts with an empty
   window, as zlib's full flush does. */
const char *sp_zlib_deflate_block(const char *src, sp_int level, sp_int final) {SP_GC_ROOT_STR(src);
  sp_zlib_err = NULL;
  size_t n = src ? sp_str_byte_len(src) : 0;
  sp_zlib_out o;
  o.p = NULL; o.len = 0; o.cap = 0; o.err = 0;
  sp_zlib_bw w;
  w.o = &o; w.buf = 0; w.cnt = 0;
  if (n > 0 || final) sp_zlib_deflate_block_into(&w, (const unsigned char *)src, n, (int)level, final != 0);
  if (!final) {
    sp_zlib_bw_put(&w, 0, 3);   /* BFINAL 0, BTYPE 00: stored */
    sp_zlib_bw_flush(&w);       /* a stored block starts on a byte */
    static const unsigned char sync[4] = { 0x00, 0x00, 0xff, 0xff };   /* LEN 0, NLEN */
    sp_zlib_out_write(&o, sync, 4);
  }
  else sp_zlib_bw_flush(&w);
  return sp_zlib_out_str(&o);
}

/* ------------------------------------------------------- inflate in pieces */

/* The bit of the input the reader is at. */
static size_t sp_zlib_in_bit(const sp_zlib_in *s) {
  return s->pos * 8 - (size_t)s->bitcnt;
}

static void sp_zlib_in_seek(sp_zlib_in *s, size_t bit) {
  if (bit > s->len * 8) { s->err = 1; return; }
  s->pos = bit / 8; s->bitbuf = 0; s->bitcnt = 0;
  if (bit % 8) sp_zlib_in_bits(s, (int)(bit % 8));
}

/* A stored block as far as the input has it, from the bit `*go` when that
   is inside its bytes. When the input ends first, `*go` is where the next
   bytes of the block go on from. */
static int sp_zlib_part_stored(sp_zlib_in *s, sp_zlib_out *o, size_t *go) {
  s->bitbuf = 0; s->bitcnt = 0;              /* stored blocks start byte-aligned */
  if (s->pos + 4 > s->len) { s->err = 1; s->eof = 1; return -1; }
  size_t len  = (size_t)s->src[s->pos] | ((size_t)s->src[s->pos + 1] << 8);
  size_t nlen = (size_t)s->src[s->pos + 2] | ((size_t)s->src[s->pos + 3] << 8);
  if ((len ^ 0xffff) != nlen) { s->err = 1; return -1; }
  size_t data = s->pos + 4;
  size_t from = *go ? *go / 8 : data;
  if (from < data || from > data + len || from > s->len) { s->err = 1; return -1; }
  size_t left = data + len - from;
  size_t n = s->len - from < left ? s->len - from : left;
  if (n) sp_zlib_out_write(o, s->src + from, n);
  if (o->err) return -1;
  s->pos = from + n;
  if (n < left) { *go = s->pos * 8; s->err = 1; s->eof = 1; return -1; }
  return 0;
}

/* Where the last sp_zlib_inflate_part stopped: the bit its unfinished
   block starts at (past the final block, the bit after it), the bit inside
   that block to go on from (0 for its start), and whether the final block
   ended. Read right after the call, as the error message is. */
static SP_TLS sp_int sp_zlib_part_at[3];

sp_int sp_zlib_inflate_at(sp_int which) {
  return which >= 0 && which < 3 ? sp_zlib_part_at[which] : 0;
}

/* Zlib::Inflate over a stream that comes in pieces: the raw deflate data
   in `src` as far as it goes, from the block at `bit`, or from `go` inside
   that block when an earlier call stopped there. `window` is the output
   before this call that a match may reach back into, and is not answered
   again. No wrapper: the Ruby side reads the header and checks the
   trailer. Input that ends early is not an error here, since the next
   piece continues it; NULL is a malformed stream. */
const char *sp_zlib_inflate_part(const char *src, sp_int bit, sp_int go, const char *window) {SP_GC_ROOT_STR(src);SP_GC_ROOT_STR(window);
  sp_zlib_err = NULL;
  size_t wn = window ? sp_str_byte_len(window) : 0;
  sp_zlib_in s;
  s.src = (const unsigned char *)src; s.len = src ? sp_str_byte_len(src) : 0; s.pos = 0;
  s.bitbuf = 0; s.bitcnt = 0; s.err = 0; s.eof = 0;
  sp_zlib_out o;
  o.p = NULL; o.len = 0; o.cap = 0; o.err = 0;
  if (wn) sp_zlib_out_write(&o, (const unsigned char *)window, wn);

  size_t at = bit < 0 ? 0 : (size_t)bit;
  size_t resume = go < 0 ? 0 : (size_t)go;
  int ended = 0;
  sp_zlib_in_seek(&s, at);
  while (!s.err && !o.err) {
    at = sp_zlib_in_bit(&s);
    int final = sp_zlib_in_bits(&s, 1); if (final < 0) break;
    int type  = sp_zlib_in_bits(&s, 2); if (type  < 0) break;
    int rc;
    if (type == 0) rc = sp_zlib_part_stored(&s, &o, &resume);
    else if (type == 3) { s.err = 1; break; }
    else {
      sp_zlib_huff lit, dist;
      if (type == 1) sp_zlib_fixed_tables(&lit, &dist);
      else if (sp_zlib_dynamic_tables(&s, &lit, &dist) < 0) break;
      if (resume) sp_zlib_in_seek(&s, resume);
      for (;;) {
        sp_zlib_in before = s;
        rc = sp_zlib_block_code(&s, &o, &lit, &dist);
        if (rc < 0 && s.eof) resume = sp_zlib_in_bit(&before);
        if (rc) break;
      }
    }
    if (rc < 0) break;
    resume = 0;
    if (final) { ended = 1; at = sp_zlib_in_bit(&s); break; }
  }

  if (o.err || (s.err && !s.eof)) {
    sp_zlib_err = o.err ? "out of memory" : "invalid deflate data";
    free(o.p);
    return NULL;
  }
  sp_zlib_part_at[0] = (sp_int)at;
  sp_zlib_part_at[1] = (sp_int)resume;
  sp_zlib_part_at[2] = ended;
  if (wn) memmove(o.p, o.p + wn, o.len - wn);
  o.len -= wn;
  return sp_zlib_out_str(&o);
}

/* ------------------------------------------------------------- checksum API */

/* the checksums are unsigned 32-bit values, past a 32-bit sp_int's range:
   answered as a boxed Integer (a Bignum there, the plain value on 64-bit) */
sp_RbVal sp_zlib_crc32_of(const char *src, sp_RbVal init) {SP_GC_ROOT_STR(src);
  size_t n = src ? sp_str_byte_len(src) : 0;
  return sp_box_i64((int64_t)sp_zlib_crc32((uint32_t)sp_unbox_i64(init), (const unsigned char *)src, n));
}

sp_RbVal sp_zlib_adler32_of(const char *src, sp_RbVal init) {SP_GC_ROOT_STR(src);
  size_t n = src ? sp_str_byte_len(src) : 0;
  return sp_box_i64((int64_t)sp_zlib_adler32((uint32_t)sp_unbox_i64(init), (const unsigned char *)src, n));
}

# Array#pack / String#unpack: honor the '<' (little-endian), '>'
# (big-endian), '!' and '_' (native size) directive modifiers.
# Regression: each modifier char was consumed as if it were the next
# directive, so pack('l<l<l<l<') silently dropped every other element
# (the doom gem's SDL_Rect blit came out black).

# Round-trip: all four values must survive.
p [1, 2, 3, 4].pack('l<l<l<l<').unpack('l<l<l<l<')

# Byte-level pack checks (native is little-endian).
p [0x01020304].pack('l>').bytes
p [0x01020304].pack('l<').bytes
p [0x0102].pack('s>').bytes
p [0x0102].pack('s<').bytes
p [0x0102030405060708].pack('q>').bytes
p [0x0102030405060708].pack('q<').bytes
p [-2].pack('l>').bytes

# Unsigned variants.
p [0xFFFE].pack('S>').bytes
p [0xFFFE].pack('S<').bytes
p [0xDEADBEEF].pack('L>').bytes
p [0xDEADBEEF].pack('L<').bytes
p [0x0102030405060708].pack('Q>').bytes

# '!' and '_' select the native size; for s/S (16-bit) and q/Q (64-bit)
# that matches the plain directive, and they must be consumed, not
# treated as directives themselves.
p [1, 2].pack('s!s_').bytes
p [7].pack('s!').bytes
p [9].pack('q!').bytes

# Count and '*' still parse after a modifier.
p [1, 2, 3].pack('l<3').bytes
p [1, 2].pack('S<*').bytes

# Cross-check against the fixed-endian directives.
p [0x0102].pack('n').bytes == [0x0102].pack('S>').bytes
p [0x0102].pack('v').bytes == [0x0102].pack('S<').bytes
p [0x01020304].pack('N').bytes == [0x01020304].pack('L>').bytes
p [0x01020304].pack('V').bytes == [0x01020304].pack('L<').bytes

# Unpack with modifiers.
p "\x01\x02\x03\x04".unpack('l>')
p "\x01\x02\x03\x04".unpack('l<')
p "\x01\x02".unpack('s>')
p "\xFF\xFE".unpack('s>')
p "\xFF\xFE".unpack('S>')
p "\x01\x02\x03\x04\x05\x06\x07\x08".unpack('q>')
p "\x01\x02\x03\x04\x05\x06\x07\x08".unpack('Q>')
p "\x01\x00\x02\x00\x03\x00".unpack('S<3')
p "\x2A\x00".unpack('s!')

# Mixed (poly) array path: a string element alongside the modified directive.
p ['abc', 0x01020304].pack('a3l>').bytes

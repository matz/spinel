# spinel: share
# spinel: gc-minor
# Array#pack / String#unpack modifier validation, UTF-8 U range errors,
# and StrArray float directive TypeError parity with CRuby 4.0.

def check(label)
  res = yield
  puts "#{label}: ok #{res.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

# 1. '<', '>', '!', '_' are allowed only after sSiIlLqQjJ (ArgumentError on floats & others).
%w[d> d< D> D< f> f< F> F< e> E< g< G> d! d_ f! f_ C< c> n> V< U> a! x< X> @_ m> <<].each do |fmt|
  check("pack float #{fmt}") { [1.5].pack(fmt) }
  check("pack int #{fmt}") { [1].pack(fmt) }
  check("pack poly #{fmt}") { [1, 1.5].pack(fmt) }
  check("pack buf #{fmt}") { [1, 1.5].pack(fmt, buffer: +"pre") }
  check("pack str #{fmt}") { ["x"].pack(fmt) }
  check("unpack #{fmt}") { ("\x00" * 16).unpack(fmt) }
end

# unpack1 with invalid float modifier (literal format path)
check("unpack1 d>") { ("\x00" * 8).unpack1("d>") }
check("unpack1 f<") { ("\x00" * 4).unpack1("f<") }

# 2. Repeated or conflicting endianness modifiers raise RangeError.
%w[l>< l<> l<< l>> l<!< l!>< l_<< s>< S<> i>< I<< q>< Q>> j>< J<< l><0].each do |fmt|
  check("pack #{fmt}") { [1].pack(fmt) }
  check("pack float #{fmt}") { [1.5].pack(fmt) }
  check("pack poly #{fmt}") { [1, "a"].pack(fmt) }
  check("pack buf #{fmt}") { [1, "a"].pack(fmt, buffer: +"pre") }
  check("pack str #{fmt}") { ["a"].pack(fmt) }
  check("unpack #{fmt}") { ("\x00" * 16).unpack(fmt) }
end

# Literal integer-only unpack format (sp_str_unpack_ints) must also raise RangeError.
check("unpack_ints l><") { "\x00\x00\x00\x00".unpack("l><") }
check("unpack_ints s<>") { "\x00\x00".unpack("s<>") }
check("unpack_ints l<<") { "\x00\x00\x00\x00".unpack("l<<") }
check("unpack_ints i>>") { "\x00\x00\x00\x00".unpack("i>>") }

# Valid modifier combinations on sSiIlLqQjJ keep working (32-bit & 64-bit safe).
puts "l!> roundtrip: #{[1].pack('l!>').unpack('l!>').inspect}"
puts "l>! roundtrip: #{[1].pack('l>!').unpack('l>!').inspect}"
puts "l<_ roundtrip: #{[1].pack('l<_').unpack('l<_').inspect}"
puts "l!> width: #{[1].pack('l!>').bytesize == [0].pack('J').bytesize}"
puts "s!_: #{[1].pack('s!_').bytes == [1].pack('s').bytes}"
puts "s!>: #{[1].pack('s!>').bytes.inspect}"
puts "s<!: #{[1].pack('s<!').bytes.inspect}"

# Explicit-endian float directives (e/E little-endian, g/G big-endian) round-trip cleanly.
puts "G: #{[1.5].pack('G').unpack1('H*')}"
puts "E: #{[1.5].pack('E').unpack1('H*')}"
puts "g: #{[-2.5].pack('g').unpack1('H*')}"
puts "e: #{[-2.5].pack('e').unpack1('H*')}"
puts "roundtrip G/E: #{[1.5].pack('G').unpack1('G')} #{[1.5].pack('E').unpack1('E')}"
puts "roundtrip g/e: #{[-2.5].pack('g').unpack1('g')} #{[-2.5].pack('e').unpack1('e')}"

# 3. Negative U codepoints raise RangeError across all Array pack paths.
check("int U -1") { [-1].pack("U") }
check("int U2 mid -1") { [65, -1].pack("U2") }
check("flt U -1.0") { [-1.0].pack("U") }
check("flt U -0.9") { [-0.9].pack("U").bytes }
check("poly U -1") { ["a", -1].pack("aU") }
check("poly U -1.0") { ["a", -1.0].pack("aU") }
check("buf U -1") { ["a", -1].pack("aU", buffer: +"hi") }
check("int U 0") { [0].pack("U").bytes }
check("int U max31") { [0x7FFFFFFF].pack("U").bytes }

# 4. StrArray with float directives (including F and D) and nil elements.
%w[f F d D e E g G].each do |fmt|
  check("str #{fmt}") { ["1.5"].pack(fmt) }
end
sn = ["a", "b"]
sn[0] = nil
check("str nil F") { sn.pack("F") }
check("str nil D") { sn.pack("D") }
check("str nil C") { sn.pack("C") }

# 5. Repeated error paths to verify zero heap leaks under sanitizers.
20.times do
  begin; [1].pack("CX5"); rescue ArgumentError; end
  begin; [1].pack("l><"); rescue RangeError; end
  begin; [-1].pack("U"); rescue RangeError; end
  begin; [1.5].pack("d>"); rescue ArgumentError; end
  begin; ["1.5"].pack("F"); rescue TypeError; end
  begin; ["1.5"].pack("D"); rescue TypeError; end
  begin; [Float::NAN].pack("C"); rescue FloatDomainError; end
  ia = [1, 2]; ia[0] = nil
  begin; ia.pack("d"); rescue TypeError; end
  begin; ia.pack("C"); rescue TypeError; end
  fa = [1.5, 2.5]; fa[0] = nil
  begin; fa.pack("d"); rescue TypeError; end
  begin; fa.pack("C"); rescue TypeError; end
  begin; [1, nil].pack("Cd"); rescue TypeError; end
  begin; [1, "x"].pack("CC"); rescue TypeError; end
  begin; [1, "x"].pack("CC", buffer: +"pre"); rescue TypeError; end
end
puts "leak-loop: ok"

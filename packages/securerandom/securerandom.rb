# Spinel bundled `securerandom` -- a native binding with NO carried C.
#
# The entropy is the runtime's own CSPRNG (lib/sp_crypto.c: arc4random_buf
# on BSD/macOS, getrandom(2) then /dev/urandom on Linux, raising rather than
# degrading when neither is available). This package only declares the Ruby
# surface and renders those bytes.
#
# WHY NOT Random.urandom. It is the obvious-looking primitive and it is the
# wrong one: spinel's `Random.urandom` is a PCG stream seeded from time and
# clock -- its own comment says "a deterministic-per-run stand-in" -- whereas
# CRuby's is the OS entropy source. A SecureRandom built on it would hand
# back guessable session tokens and API keys, and nothing would ever fail.
# `sp_crypto_random_bin` is the one entry point that is actually seeded from
# the kernel.
#
# Subset. Present: random_bytes/bytes, hex, base64, urlsafe_base64, uuid
# (and its uuid_v4 alias), uuid_v7, alphanumeric, and `random_number` for a
# positive Integer bound. Not modelled: `random_number`'s Float forms (no argument,
# a bound below 1, a Float or a Range), `base36`, `choose`, and the `chars:` keyword on `alphanumeric` -- all of which are
# renderings a program can write for itself over `random_bytes`, which the
# absent ones here are not.
#
# One draw is capped at SPC_RANDOM_MAX (256) bytes by the C side; longer
# requests are assembled here from repeated draws, so `hex(1000)` works and
# each chunk is independently kernel-sourced.
module SecureRandom
  # A-Z, a-z, 0-9 in CRuby's order (Random::Formatter::ALPHANUMERIC).
  ALPHANUMERIC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

  HEXDIGITS = "0123456789abcdef"

  B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

  B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

  # The largest single draw sp_crypto_random_bin serves. This number lives
  # twice -- here and as SPC_RANDOM_MAX in lib/sp_crypto.h -- which is the
  # shape that quietly rots. It cannot rot silently here: a value ABOVE the
  # C constant makes `draw` raise ArgumentError, and test/
  # securerandom_bytes.rb asks for exactly 256 and 257 bytes, so the two
  # disagreeing fails that test rather than shortening a token.
  MAX_DRAW = 256

  # The CSPRNG itself. `:cbinstr` because the result is raw bytes whose
  # length comes from sp_ffi_bin_len rather than strlen -- a NUL turns up
  # about one byte in 256, and strlen would cut the draw short there.
  native_lib "securerandom"
  native_func :draw, [:int], :cbinstr, "sp_crypto_random_bin"

  # Returns a random binary String of `n` bytes.
  def self.random_bytes(n = 16)
    raise ArgumentError, "negative string size" if n < 0
    return SecureRandom.draw(n) if n <= MAX_DRAW
    out = +""
    left = n
    while left > 0
      take = left > MAX_DRAW ? MAX_DRAW : left
      out << SecureRandom.draw(take)
      left -= take
    end
    out
  end

  # CRuby names this one too, as an alias.
  def self.bytes(n)
    SecureRandom.random_bytes(n)
  end

  # `n` random bytes as 2n lowercase hex characters.
  def self.hex(n = 16)
    bytes = SecureRandom.random_bytes(n)
    out = +""
    i = 0
    while i < bytes.bytesize
      b = bytes.getbyte(i)
      out << HEXDIGITS[(b >> 4) & 0xf]
      out << HEXDIGITS[b & 0xf]
      i += 1
    end
    out
  end

  # `n` random bytes as padded base64.
  def self.base64(n = 16)
    SecureRandom.encode64(SecureRandom.random_bytes(n), B64, true)
  end

  # `n` random bytes as base64url. Unpadded unless `padding` is true,
  # matching CRuby's default.
  def self.urlsafe_base64(n = 16, padding = false)
    SecureRandom.encode64(SecureRandom.random_bytes(n), B64URL, padding)
  end

  # A random v4 UUID. Sixteen random bytes with the six bits RFC 9562
  # fixes: the version nibble is 4, and the variant's top two bits are 10.
  # The other 122 bits are the draw.
  def self.uuid
    b = SecureRandom.random_bytes(16)
    h = +""
    i = 0
    while i < 16
      v = b.getbyte(i)
      v = (v & 0x0f) | 0x40 if i == 6
      v = (v & 0x3f) | 0x80 if i == 8
      h << HEXDIGITS[(v >> 4) & 0xf]
      h << HEXDIGITS[v & 0xf]
      i += 1
    end
    "#{h[0, 8]}-#{h[8, 4]}-#{h[12, 4]}-#{h[16, 4]}-#{h[20, 12]}"
  end

  def self.uuid_v4
    SecureRandom.uuid
  end

  def self.uuid_v7(extra_timestamp_bits: 0)
    bits = extra_timestamp_bits
    begin
      bits = 13 if bits.is_a?(Float) && bits.to_f.finite? && (bits <= -1 || bits >= 13)
      bits = Integer(bits) unless bits.is_a?(Integer)
    rescue ArgumentError
      if extra_timestamp_bits.is_a?(String)
        # CRuby's wide decimal parser ignores a trailing NUL beyond ULONG_MAX.
        nul = 0
        nul += 1 while nul < extra_timestamp_bits.bytesize && extra_timestamp_bits.getbyte(nul) != 0
        text = extra_timestamp_bits.byteslice(0, nul)
        digits = text.delete("-+_")
        if nul < extra_timestamp_bits.bytesize && text.match?(/\A[+-]?[1-9](?:_?[0-9])*\z/) &&
            (digits.length > 20 || (digits.length == 20 && digits >= "18446744073709551616"))
          raise ArgumentError, "extra_timestamp_bits must be in 0..12"
        end
        raise ArgumentError, "invalid value for Integer(): #{extra_timestamp_bits.inspect}"
      end
      raise
    rescue RangeError => error
      raise unless error.message.start_with?("bignum too big", "integer overflow parsing", "float out of Integer range")
      bits = 13
    end
    # Select a small width without narrowing a Bignum through Integer or case.
    width = 0
    width += 1 while width < 13 && bits != width
    case width
    when 0 # min timestamp precision
      ms = Process.clock_gettime(Process::CLOCK_REALTIME, :millisecond)
      rand = SecureRandom.random_bytes(10)
      rand.setbyte(0, rand.getbyte(0) & 0x0f | 0x70) # version
      rand.setbyte(2, rand.getbyte(2) & 0x3f | 0x80) # variant
      "%08x-%04x-%s" % [(ms & 0x0000_ffff_ffff_0000) >> 16,
        ms & 0xffff, rand.unpack("H4H4H12").join("-")]
    when 12 # max timestamp precision
      ms, ns = Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond).divmod(1_000_000)
      extra_bits = ns * 4096 / 1_000_000
      rand = SecureRandom.random_bytes(8)
      rand.setbyte(0, rand.getbyte(0) & 0x3f | 0x80) # variant
      "%08x-%04x-7%03x-%s" % [(ms & 0x0000_ffff_ffff_0000) >> 16,
        ms & 0xffff, extra_bits, rand.unpack("H4H12").join("-")]
    when (0..12) # the generic version is slower than the special cases above
      rand_a, rand_b1, rand_b2, rand_b3 = SecureRandom.random_bytes(10).unpack("nnnN")
      rand_mask_bits = 12 - width
      ms, ns = Process.clock_gettime(Process::CLOCK_REALTIME, :nanosecond).divmod(1_000_000)
      "%08x-%04x-%04x-%04x-%04x%08x" % [(ms & 0x0000_ffff_ffff_0000) >> 16, ms & 0xffff,
        0x7000 | ((ns * (1 << width) / 1_000_000) << rand_mask_bits) |
          rand_a & ((1 << rand_mask_bits) - 1), 0x8000 | (rand_b1 & 0x3fff), rand_b2, rand_b3]
    else
      raise ArgumentError, "extra_timestamp_bits must be in 0..12"
    end
  end

  # `n` characters from A-Z, a-z, 0-9, uniformly.
  #
  # REJECTION SAMPLING, not `byte % 62`. 256 is not a multiple of 62, so the
  # modulo would make the first eight letters of the alphabet about 5% more
  # likely than the rest -- invisible in every test that checks the character
  # set, and a real reduction in the entropy of a session token. Bytes at or
  # above 248 (the largest multiple of 62 that fits) are thrown away; that is
  # 8 in 256, so a draw of 2n covers n characters comfortably and the loop
  # goes back for more when it does not.
  #
  # CRuby reaches the same distribution by a different route (base-62 digits
  # of one bounded draw, `Random::Formatter#choose`). The output is
  # indistinguishable; only the number of bytes consumed differs.
  # A NEGATIVE n answers "", it does not raise -- CRuby's `choose` loops
  # `while m <= n` and simply never enters, and `random_bytes(-1)` raising
  # while `alphanumeric(-1)` does not is the sort of asymmetry a port
  # invents by being tidier than the original. Measured, not assumed.
  def self.alphanumeric(n = 16)
    out = +""
    while out.length < n
      want = n - out.length
      bytes = SecureRandom.random_bytes(want * 2 + 8)
      i = 0
      while i < bytes.bytesize && out.length < n
        b = bytes.getbyte(i)
        out << ALPHANUMERIC[b % 62] if b < 248
        i += 1
      end
    end
    out
  end

  # A uniform Integer in 0...n, for a positive Integer n (a one-time code:
  # `random_number(10**6)`). UNBIASED, which `bytes % n` alone is not: the
  # draw takes just enough bytes for n - 1, masks the top byte down to its
  # bit length, and is rejected and repeated when it lands at n or above --
  # fewer than two draws on average. A code drawn with a modulo bias would
  # still look random and would still be measurably easier to guess. The
  # value never passes 2**bit_length(n - 1), so it stays inside the
  # Integer on a 32-bit target too; a Bignum bound is served under
  # --int-overflow=promote, as any Bignum arithmetic is, and raises
  # RangeError under raise.
  # CRuby's Float forms (no argument, 0 or a negative bound, a Float, a
  # Range) are not modelled; a bound below 1 raises NotImplementedError
  # rather than answer an Integer where CRuby answers a Float.
  def self.random_number(n)
    raise NotImplementedError, "SecureRandom.random_number answers a Float for a bound below 1, which is not modelled" if n < 1
    bits = (n - 1).bit_length
    size = (bits + 7) / 8
    top_mask = (1 << (bits - 8 * (size - 1))) - 1
    while true
      bytes = SecureRandom.random_bytes(size)
      value = 0
      i = 0
      while i < size
        b = bytes.getbyte(i)
        b &= top_mask if i == 0
        value = value * 256 + b
        i += 1
      end
      return value if value < n
    end
  end

  # Base64 over an explicit alphabet, so the standard and URL-safe forms are
  # one implementation. Written out rather than delegating to the `base64`
  # package: that would make this package's entropy depend on another
  # package being required.
  def self.encode64(bytes, alphabet, padding)
    out = +""
    n = bytes.bytesize
    i = 0
    while i + 3 <= n
      v = (bytes.getbyte(i) << 16) | (bytes.getbyte(i + 1) << 8) | bytes.getbyte(i + 2)
      out << alphabet[(v >> 18) & 0x3f]
      out << alphabet[(v >> 12) & 0x3f]
      out << alphabet[(v >> 6) & 0x3f]
      out << alphabet[v & 0x3f]
      i += 3
    end
    left = n - i
    if left == 1
      v = bytes.getbyte(i) << 16
      out << alphabet[(v >> 18) & 0x3f]
      out << alphabet[(v >> 12) & 0x3f]
      out << "==" if padding
    elsif left == 2
      v = (bytes.getbyte(i) << 16) | (bytes.getbyte(i + 1) << 8)
      out << alphabet[(v >> 18) & 0x3f]
      out << alphabet[(v >> 12) & 0x3f]
      out << alphabet[(v >> 6) & 0x3f]
      out << "=" if padding
    end
    out
  end
end

# OpenSSL::Cipher -- CRuby's stateful object, over stateless C.
#
# The object a program writes is CRuby's, because that is what applications
# really write:
#
#   c = OpenSSL::Cipher.new("aes-128-gcm")
#   c.encrypt; c.key = k; c.iv = n; c.auth_data = ad
#   ct = c.update(plaintext) + c.final
#   tag = c.auth_tag
#
# The input is held as ordinary ivars over stateless C (#4221). Each update
# XORs its bytes with a private GCM keystream, the encryption of zero bytes.
# Growing that cache geometrically makes the total work linear in the input.
# The counter-mode transform is the same in either direction and uses the
# native call's IV handling. Input and keystream remain buffered, not bounded
# in memory. There is no EVP context to free, handle table, or new verdict path.
# Final still makes one whole-message call; aes_gcm_decrypt checks the tag
# before the operation succeeds. As in CRuby, plaintext returned by update is
# unauthenticated until then. The keystream is cleared even if final rejects.
#
# THE ALGORITHM SET IS aes-128-gcm AND aes-256-gcm, and any other name raises.
# Cipher.new's argument is an algorithm chosen by a runtime string, which
# openssl/digest.rb refuses for Digest -- "an algorithm chosen by a runtime
# string cannot resolve to a C function at compile time". The two are the same
# rule seen from both sides: EVP_get_cipherbyname is ONE C entry point that
# resolves the whole family, where sp_crypto's digests are one C function per
# algorithm. That is also why the set is a list rather than whatever EVP
# knows: a mode nothing here has run against vectors must not start working by
# accident. Widen it when something real needs a mode, with vectors.
module OpenSSL
  class CipherError < OpenSSLError
  end

  class Cipher
    # name => [key bytes, iv bytes]. GCM's 12-byte IV is the one every
    # protocol that names AES-GCM uses; another length is accepted, since GCM
    # defines them, but this is what #random_iv answers.
    CIPHERS = {
      "aes-128-gcm" => [16, 12],
      "aes-256-gcm" => [32, 12],
    }

    # [s].join copies a String's bytes without running a subclass's dup or to_s,
    # so what's retained here can't change after it's set.
    def name = [@name].join

    def initialize(name)
      @name = [name.to_s].join.downcase
      unless CIPHERS.key?(@name)
        raise CipherError,
              "unsupported cipher: #{name} (this package has #{CIPHERS.keys.join(", ")})"
      end
      @encrypting = true
      @key = ""
      @iv = ""
      @iv_len = nil
      @auth_data = ""
      @parts = []
      @emitted = 0
      @keystream = ""
      @auth_tag = ""
      @expected_tag = ""
      @done = false
    end

    # CRuby returns self from both, so `c.encrypt; ...` and `c.encrypt.key = k`
    # both work.
    def encrypt
      @encrypting = true
      self
    end

    def decrypt
      @encrypting = false
      self
    end

    def key_len = CIPHERS[@name][0]
    # the iv length #iv= takes: the cipher's own (12 for GCM) unless the
    # program set another with #iv_len=, as CRuby's GCM takes any
    def iv_len  = @iv_len || CIPHERS[@name][1]

    def iv_len=(n)
      n = n.to_int
      raise ArgumentError, "iv_len must be positive" unless n > 0
      @iv_len = n
      n
    end

    # A key or an iv of the wrong length is CRuby's ArgumentError, with its
    # message ("key must be 16 bytes"), not a CipherError.
    def key=(k)
      raise ArgumentError, "key must be #{key_len} bytes" unless k.bytesize == key_len
      @key = [k].join
      restart
      k
    end

    def iv=(v)
      raise ArgumentError, "iv must be #{iv_len} bytes" unless v.bytesize == iv_len
      @iv = [v].join
      restart
      v
    end

    # Setting the key or the iv starts a new message, as CRuby's EVP re-init
    # does: the input updates gave before it is no part of the message final
    # seals, whose bytes they already returned under the old setting, and a
    # cipher final used up takes a new one the same way.
    private def restart
      @parts = []
      @emitted = 0
      @keystream = ""
      @done = false
    end

    # Covered by the tag, absent from the output.
    def auth_data=(a)
      @auth_data = [a].join
      a
    end

    # The tag to check, on the decrypt side. CRuby takes it before #final,
    # because #final is what verifies it.
    def auth_tag=(t)
      unless t.bytesize == 16
        raise CipherError, "auth tag must be 16 bytes"
      end
      @expected_tag = [t].join
      t
    end

    def random_key
      self.key = Crypto.random_bin(key_len)
    end

    def random_iv
      self.iv = Crypto.random_bin(iv_len)
    end

    # Keep the bytes consumed by each call, even if the caller changes its input.
    def update(data)
      raise CipherError, "cipher already finished" if @done
      part = [data].join
      needed = @emitted + part.bytesize
      if needed > @keystream.bytesize || @keystream.empty?
        # The one-shot native update takes an int length.
        length = [needed, [2 * @keystream.bytesize, 2_147_483_647].min].max
        zeros = "\0" * length
        out = Native.aes_gcm_encrypt(@name, @key, @iv, @auth_data, zeros)
        raise CipherError, reason("AES-GCM update failed") if out.empty?
        @keystream = out.byteslice(0, length)
      end
      chunk = String.new(encoding: Encoding::BINARY)
      i = 0
      while i < part.bytesize
        chunk << (part.getbyte(i) ^ @keystream.getbyte(@emitted + i))
        i += 1
      end
      @parts << part
      @emitted = needed
      chunk
    end

    # The whole operation, in one C call.
    def final
      raise CipherError, "cipher already finished" if @done
      raise CipherError, "key not set" if @key.empty?
      raise CipherError, "iv not set" if @iv.empty?
      @done = true
      @keystream = ""
      data = @parts.join

      if @encrypting
        # Encrypt needs no status byte: the answer always carries a 16-byte
        # tag, so empty is unambiguously a failure.
        out = Native.aes_gcm_encrypt(@name, @key, @iv, @auth_data, data)
        raise CipherError, reason("AES-GCM encrypt failed") if out.empty?
        # ciphertext || tag: the C returns both in one value, and the tag is
        # always the last 16 bytes.
        @auth_tag = out.byteslice(out.bytesize - 16, 16)
        out.byteslice(@emitted, out.bytesize - 16 - @emitted)
      else
        if @expected_tag.empty?
          raise CipherError, "auth_tag must be set before final on a decrypt"
        end
        out = Native.aes_gcm_decrypt(@name, @key, @iv, @auth_data, data, @expected_tag)
        # The verdict is the answer's own first byte, not a second call: an
        # empty string is a legitimate plaintext, so it cannot also mean
        # "forged", and an out-of-band verdict can be read after another
        # thread has overwritten it. last_error is only the wording.
        raise CipherError, reason("AES-GCM decrypt failed") if out.empty?
        out.byteslice(1 + @emitted, out.bytesize - 1 - @emitted)
      end
    end

    # Available after #final on the encrypt side, as in CRuby.
    def auth_tag
      raise CipherError, "auth_tag is not available until final" unless @done
      [@auth_tag].join
    end

    private

    # last_error is per-thread, so this is the caller's own reason; it is the
    # wording of a failure the return value has already established, never the
    # test for whether one happened.
    def reason(fallback)
      err = Native.last_error
      err.empty? ? fallback : err
    end
  end
end

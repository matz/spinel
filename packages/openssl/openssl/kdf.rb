# OpenSSL::KDF.hkdf -- HKDF (RFC 5869) -- and OpenSSL::KDF.pbkdf2_hmac /
# OpenSSL::PKCS5.pbkdf2_hmac -- PBKDF2 (RFC 8018), CRuby's spellings and keywords.
#
# No C: HKDF is HMAC over raw bytes, extract then expand, and the raw HMAC
# spelling landed beside this file. Writing it in Ruby keeps it out of the
# OPENSSL_AVAILABLE gate -- a program that reaches HKDF through this package
# gets it whether or not the build found libcrypto's headers, the same way
# OpenSSL::Digest works.
module OpenSSL
  module KDF
    class KDFError < OpenSSLError
    end

    # Extract-then-expand, in one call as CRuby has it. `salt` is optional in
    # the RFC and defaults to a string of zeros the length of the hash; CRuby
    # requires the keyword, so an explicit "" gets the RFC's default here.
    #
    # The length ceiling is the RFC's: expand emits 255 hash-lengths at most,
    # because the counter it appends is one octet.
    def self.hkdf(ikm, salt:, info:, length:, hash:)
      algo = HMAC.algorithm(hash)
      hlen = Digest.length_of(algo)
      raise KDFError, "length must be positive" if length <= 0
      raise KDFError, "length exceeds #{255 * hlen} for #{algo}" if length > 255 * hlen

      prk = HMAC.digest(algo, salt.empty? ? "\0" * hlen : salt, ikm)

      blocks = []
      block = ""
      have = 0
      counter = 1
      while have < length
        block = HMAC.digest(algo, prk, block + info + counter.chr)
        blocks << block
        have += block.bytesize
        counter += 1
      end
      # ASCII-8BIT, as CRuby answers: this is keying material, and a caller
      # comparing it against bytes off a wire needs the tag to agree.
      blocks.join.byteslice(0, length).b
    end

    # PBKDF2 (RFC 8018) over HMAC, CRuby's keywords; `hash` is a name or a
    # Digest. In Ruby over the raw HMAC, as hkdf is.
    def self.pbkdf2_hmac(pass, salt:, iterations:, length:, hash:)
      algo = hash.is_a?(Digest) ? hash.name : hash.to_s.upcase
      hlen = case algo
             when "SHA256" then 32
             when "SHA1"   then 20
             else raise Digest::DigestError, "unsupported digest algorithm: #{hash}"
             end
      raise KDFError, "iterations must be positive" if iterations <= 0
      out = []
      have = 0
      i = 1
      while have < length
        u = HMAC.digest(algo, pass, salt + [i].pack("N"))
        t = u.bytes
        (iterations - 1).times do
          u = HMAC.digest(algo, pass, u)
          k = 0
          while k < hlen
            t[k] = t[k] ^ u.getbyte(k)
            k += 1
          end
        end
        out << t.pack("C*")
        have += hlen
        i += 1
      end
      out.join.byteslice(0, length).b
    end
  end

  # CRuby's older spelling of the same derivation: positional arguments, the
  # digest last.
  module PKCS5
    def self.pbkdf2_hmac(pass, salt, iter, keylen, digest)
      KDF.pbkdf2_hmac(pass, salt: salt, iterations: iter, length: keylen, hash: digest)
    end

    def self.pbkdf2_hmac_sha1(pass, salt, iter, keylen)
      KDF.pbkdf2_hmac(pass, salt: salt, iterations: iter, length: keylen, hash: "SHA1")
    end
  end
end

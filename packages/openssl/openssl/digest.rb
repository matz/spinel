# OpenSSL::Digest and OpenSSL::HMAC -- the names a CRuby program writes for a
# hash or signature. SHA1/SHA256 and MD5 digests use the runtime functions
# (lib/sp_crypto.c); SHA384/SHA512 use platform libcrypto functions.
#
# The class-method forms (`OpenSSL::Digest::SHA256.hexdigest(data)`) and the
# object (`OpenSSL::Digest.new("SHA256")`, `OpenSSL::Digest::SHA256.new`)
# with update / << / digest / hexdigest / base64digest and their `!` forms /
# reset / digest_length / length / size / name / new / file / to_s / inspect /
# ==, and the class methods with the algorithm first
# (`OpenSSL::Digest.hexdigest("SHA256", data)`). Runtime hashes keep incremental
# state; SHA384/SHA512 retain input for one-shot finalization.
module OpenSSL
  # CRuby's base for everything in this namespace; SSLError and DigestError
  # both descend from it, so `rescue OpenSSL::OpenSSLError` catches either.
  class OpenSSLError < StandardError
  end

  class HMACError < OpenSSLError
  end

  module Crypto
    native_lib "openssl"
    native_func :sha256_hex,      [:string],          :cstring, "sp_crypto_sha256_hex"
    native_func :sha256_bin,      [:string],          :cbinstr, "sp_crypto_sha256_bin"
    native_func :sha1_hex,        [:string],          :cstring, "sp_crypto_sha1_hex"
    native_func :sha1_bin,        [:string],          :cbinstr, "sp_crypto_sha1_bin"
    native_func :md5_hex,         [:string],          :cstring, "sp_crypto_md5_hex"
    native_func :md5_bin,         [:string],          :cbinstr, "sp_crypto_md5_bin"
    native_func :sha256_b64,      [:string],          :cstring, "sp_crypto_sha256_b64"
    native_func :sha1_b64,        [:string],          :cstring, "sp_crypto_sha1_b64"
    native_func :md5_b64,         [:string],          :cstring, "sp_crypto_md5_b64"
    native_func :sha256_update,   [:string, :string], :cbinstr, "sp_crypto_sha256_update"
    native_func :sha256_final,    [:string],          :cbinstr, "sp_crypto_sha256_final"
    native_func :sha1_update,     [:string, :string], :cbinstr, "sp_crypto_sha1_update"
    native_func :sha1_final,      [:string],          :cbinstr, "sp_crypto_sha1_final"
    native_func :md5_update,      [:string, :string], :cbinstr, "sp_crypto_md5_update"
    native_func :md5_final,       [:string],          :cbinstr, "sp_crypto_md5_final"
    native_func :hmac_sha256_hex, [:string, :string], :cstring, "sp_crypto_hmac_sha256_hex"
    native_func :hmac_sha1_hex,   [:string, :string], :cstring, "sp_crypto_hmac_sha1_hex"
    native_func :hmac_sha256_bin, [:string, :string], :cbinstr, "sp_crypto_hmac_sha256_bin"
    native_func :hmac_sha1_bin,   [:string, :string], :cbinstr, "sp_crypto_hmac_sha1_bin"
    # The CSPRNG the runtime already carries, for Cipher#random_key /
    # #random_iv. Nothing here reaches libssl, so a key or an IV is drawn
    # from the same source `securerandom` uses rather than a second one.
    native_func :random_bin,      [:int],             :cbinstr, "sp_crypto_random_bin"
  end

  class Digest
    class DigestError < OpenSSLError
    end

    # The one-shot hash of `data` under algorithm `name` (as CRuby spells it,
    # any case), raw; the hashes the runtime carries, and DigestError for the
    # rest, the class CRuby raises for an unknown algorithm.
    def self.raw(name, data)
      case name.to_s.upcase
      when "SHA256" then Crypto.sha256_bin(data)
      when "SHA1"   then Crypto.sha1_bin(data)
      when "MD5"    then Crypto.md5_bin(data)
      when "SHA384", "SHA512" then Native.digest_bytes(name, data)
      else raise DigestError, "Unsupported digest algorithm (#{name}).: unknown object name"
      end
    end

    def self.length_of(name)
      raise ArgumentError, "string contains null byte" if name.include?("\0")
      case name.to_s.upcase
      when "SHA256" then 32
      when "SHA1"   then 20
      when "MD5"    then 16
      when "SHA384" then 48
      when "SHA512" then 64
      else
        Native.digest_bytes(name, "")
        raise DigestError, "unsupported digest algorithm: #{name}"
      end
    end

    # The class-method forms name the algorithm first, except file, which
    # takes the path first; each subclass answers them for its own algorithm.
    def self.digest(name, data)       = new(name).digest(data)
    def self.hexdigest(name, data)    = new(name).hexdigest(data)
    def self.base64digest(name, data) = new(name).base64digest(data)
    def self.file(path, name)         = new(name).file(path)

    # SHA384/SHA512 append to an owned buffer and hash it once at finish.
    # Storage remains proportional to input size; copies own their buffers.
    def self.step(name, state, data)
      case name
      when "SHA256" then Crypto.sha256_update(state, data)
      when "SHA1"   then Crypto.sha1_update(state, data)
      when "SHA384", "SHA512"
        # StringValue bytes avoid Integer codepoints and encoding conflicts.
        state << String.new(OpenSSL.send(:comparison_string, data), encoding: Encoding::BINARY)
      else Crypto.md5_update(state, data)
      end
    end

    def self.finish(name, state)
      case name
      when "SHA256" then Crypto.sha256_final(state)
      when "SHA1"   then Crypto.sha1_final(state)
      when "SHA384", "SHA512" then Native.digest_bytes(name, state)
      else Crypto.md5_final(state)
      end
    end

    def initialize(name, data = nil)
      @name = name.to_s.upcase
      Digest.length_of(@name)
      @state = String.new(encoding: Encoding::BINARY)
      update(data) if data
    end

    def initialize_copy(other)
      super
      @state = @state.dup if @name == "SHA384" || @name == "SHA512"
    end

    def name = @name
    def digest_length = Digest.length_of(@name)
    def block_length = (@name == "SHA384" || @name == "SHA512") ? 128 : 64

    def update(data)
      @state = Digest.step(@name, @state, data)
      self
    end
    alias << update

    def reset
      @state = String.new(encoding: Encoding::BINARY)
      self
    end

    # With `data`, the digest of `data` alone, and the object is reset.
    def digest(data = nil)
      return Digest.finish(@name, @state) unless data
      reset
      Digest.raw(@name, data)
    end

    # unpack1("H*") answers a String, but only a numeric directive types the
    # call, so its answer is boxed; the interpolation types it. #to_s is
    # #hexdigest, and in a program that requires openssl every `x.to_s` on a
    # boxed receiver takes #to_s's type too.
    def hexdigest(data = nil) = "#{digest(data).unpack1("H*")}"
    def base64digest(data = nil) = [digest(data)].pack("m0")

    # The `!` forms answer the digest and reset the object.
    def digest!
      value = digest
      reset
      value
    end

    def hexdigest! = "#{digest!.unpack1("H*")}"
    def base64digest! = [digest!].pack("m0")

    alias length digest_length
    alias size digest_length

    # A fresh object for the same algorithm.
    def new = clone.reset

    def to_s = hexdigest
    def inspect = "#<#{self.class.name}: #{hexdigest}>"

    # Another digest object compares by its digest, anything with to_str by
    # the hex digest, and the rest is not equal.
    def ==(other)
      return digest == other.digest if other.is_a?(Digest)
      other.respond_to?(:to_str) && hexdigest == other.to_str
    end

    # Read 16 KiB at a time; SHA384/SHA512 retain the input until reset.
    def file(name)
      File.open(name.respond_to?(:to_path) ? name.to_path : name, "rb") do |f|
        while (chunk = f.read(16_384))
          update(chunk)
        end
      end
      self
    end

    class SHA256 < Digest
      def initialize(data = nil) = super("SHA256", data)
      def self.hexdigest(data)    = Crypto.sha256_hex(data)
      def self.digest(data)       = Crypto.sha256_bin(data)
      def self.base64digest(data) = Crypto.sha256_b64(data)
      def self.file(name)         = new.file(name)
    end

    class SHA1 < Digest
      def initialize(data = nil) = super("SHA1", data)
      def self.hexdigest(data)    = Crypto.sha1_hex(data)
      def self.digest(data)       = Crypto.sha1_bin(data)
      def self.base64digest(data) = Crypto.sha1_b64(data)
      def self.file(name)         = new.file(name)
    end

    class SHA384 < Digest
      def initialize(data = nil) = super("SHA384", data)
      def self.hexdigest(data)    = new.hexdigest(data)
      def self.digest(data)       = new.digest(data)
      def self.base64digest(data) = new.base64digest(data)
      def self.file(name)         = new.file(name)
    end

    class SHA512 < Digest
      def initialize(data = nil) = super("SHA512", data)
      def self.hexdigest(data)    = new.hexdigest(data)
      def self.digest(data)       = new.digest(data)
      def self.base64digest(data) = new.base64digest(data)
      def self.file(name)         = new.file(name)
    end

    # A legacy hash, carried because Active Storage's direct upload checks a
    # blob by its MD5 (#4631). Not for new designs.
    class MD5 < Digest
      def initialize(data = nil) = super("MD5", data)
      def self.hexdigest(data)    = Crypto.md5_hex(data)
      def self.digest(data)       = Crypto.md5_bin(data)
      def self.base64digest(data) = Crypto.md5_b64(data)
      def self.file(name)         = new.file(name)
    end
  end

  # A class, as CRuby's is: the one-shot `HMAC.digest` / `HMAC.hexdigest`
  # below, and an instance a key is set on once and data fed to
  # (`OpenSSL::HMAC.new(key, "SHA256")`, `#update` / `#<<`, `#digest` /
  # `#hexdigest`), which Active Support's MessageVerifier and Action
  # Controller's CSRF token use, keying once and `dup`-ing per message.
  class HMAC
    # The algorithm is checked where the key is set, as CRuby checks it;
    # the data is kept and the MAC computed over all of it when asked, over
    # the same one-shot functions. #update answers a new String rather than
    # appending, so a `dup` (whose ivars are the original's) never shares
    # the data it goes on to add to.
    def initialize(key, digest)
      @algo = HMAC.algorithm(digest)
      unless @algo == "SHA256" || @algo == "SHA1" || @algo == "SHA384" || @algo == "SHA512"
        raise Digest::DigestError, "unsupported digest algorithm: #{digest}"
      end
      @key = OpenSSL.send(:comparison_string, key)
      @data = ""
    end

    def update(data)
      @data = @data + OpenSSL.send(:comparison_string, data)
      self
    end

    def <<(data) = update(data)

    def digest = HMAC.digest(@algo, @key, @data)
    def hexdigest = HMAC.hexdigest(@algo, @key, @data)
    def base64digest = [digest].pack("m0")
    def to_s = hexdigest

    # As CRuby: inspect shows the MAC (never the key or the data), and
    # two HMAC objects are equal when their MACs are equal. Anything that
    # is not an HMAC is not equal, a String with the same hex included.
    # (CRuby's is an alias of this class's #hexdigest, so a subclass's own
    # #hexdigest does not change it; the interpolation types it a String)
    def inspect = "#{HMAC.hexdigest(@algo, @key, @data)}"

    def ==(other)
      return false unless HMAC === other
      mine = digest
      theirs = other.digest
      return false unless mine.bytesize == theirs.bytesize
      OpenSSL.fixed_length_secure_compare(mine, theirs)
    end

    def reset
      @data = ""
      self
    end

    # CRuby takes the algorithm first, as a String or a Digest instance
    # (`OpenSSL::Digest.new("sha256")`, whose name is the algorithm), and
    # the runtime hashes and libcrypto's SHA384/SHA512 are here. An algorithm not
    # one of them raises DigestError -- the class CRuby raises, so
    # `rescue OpenSSL::Digest::DigestError` catches the same thing -- rather
    # than answering a hash from the wrong function. HMAC-MD5 is one of
    # those: CRuby has it, the runtime's crypto does not (its MD5 is the
    # digest alone).
    def self.hexdigest(algo, key, data)
      algo = HMAC.algorithm(algo)
      case algo
      when "SHA256" then Crypto.hmac_sha256_hex(key, data)
      when "SHA1"   then Crypto.hmac_sha1_hex(key, data)
      when "SHA384", "SHA512" then platform_digest(algo, key, data).unpack1("H*")
      else raise Digest::DigestError, "unsupported digest algorithm: #{algo}"
      end
    end

    # The same MAC as its own bytes. `Digest::SHA256` has had both spellings
    # since this file was written; HMAC had only the hex one, which is the
    # half a human or a header reads.
    #
    # The raw half is what another primitive reads. HKDF (RFC 5869) is HMAC
    # over raw bytes twice, and a caller with only hex has to decode between
    # the two rounds -- a lossless detour with a chance to get the decode
    # wrong. The same is true of anything comparing a MAC to bytes off a
    # wire: a webhook signature, a JWT's HS256 half, an AWS SigV4 chain.
    #
    # Same algorithm set and the same refusal as `hexdigest`: whatever one
    # answers, the other answers.
    def self.digest(algo, key, data)
      algo = HMAC.algorithm(algo)
      case algo
      when "SHA256" then Crypto.hmac_sha256_bin(key, data)
      when "SHA1"   then Crypto.hmac_sha1_bin(key, data)
      when "SHA384", "SHA512" then platform_digest(algo, key, data)
      else raise Digest::DigestError, "unsupported digest algorithm: #{algo}"
      end
    end

    # The algorithm's name, upcased: a Digest instance's own (only its
    # algorithm counts, not what it was given to hash), or the String's.
    def self.algorithm(algo)
      name = algo.is_a?(Digest) ? algo.name : algo.to_s
      Digest.length_of(name)
      name.upcase
    end

    def self.platform_digest(algo, key, data)
      key = OpenSSL.send(:comparison_string, key)
      data = OpenSSL.send(:comparison_string, data)
      Native.hmac_bytes(algo, key, data)
    end
    private_class_method :platform_digest
  end
end

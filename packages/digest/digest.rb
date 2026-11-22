# Spinel bundled `digest` -- a native binding with NO carried C.
#
# The hash implementations are the runtime's vendored crypto (lib/sp_crypto.c,
# always in libspinel_rt.a because the string/net runtime uses it); this
# package declares the CRuby-shaped Ruby surface (`Digest::Instance`,
# `Digest::Class`, `Digest::Base`, and the built-in digest classes).
module Digest
  # Digest.hexencode, the module function CRuby's digest extension defines:
  # a String's bytes as lowercase hex, two digits a byte.
  def self.hexencode(string) = "#{string.unpack1("H*")}"

  def self.string_bytes(value)
    case value
    when String then return value
    when nil, true, false then raise TypeError, "no implicit conversion of #{value.inspect} into String"
    end
    if value.respond_to?(:to_str)
      converted = value.to_str
      return converted if converted.is_a?(String)
      raise TypeError, "can't convert #{value.class} to String (#{value.class}#to_str gives #{converted.class})"
    end
    raise TypeError, "no implicit conversion of #{value.class} into String"
  end
  private_class_method :string_bytes

  # The runtime's one-shot and incremental hashing (lib/sp_crypto.c): a state
  # is the chaining value, the byte count and the partial block, in a fixed-size
  # binary String (88..208 bytes); the empty String is the initial state. Each
  # update answers a new state rather than changing the one given.
  module Native
    native_lib "digest"
    native_func :sha256_hex,    [:string],          :cstring, "sp_crypto_sha256_hex"
    native_func :sha256_bin,    [:string],          :cbinstr, "sp_crypto_sha256_bin"
    native_func :sha256_b64,    [:string],          :cstring, "sp_crypto_sha256_b64"
    native_func :sha256_update, [:string, :string], :cbinstr, "sp_crypto_sha256_update"
    native_func :sha256_final,  [:string],          :cbinstr, "sp_crypto_sha256_final"
    native_func :sha384_hex,    [:string],          :cstring, "sp_crypto_sha384_hex"
    native_func :sha384_bin,    [:string],          :cbinstr, "sp_crypto_sha384_bin"
    native_func :sha384_b64,    [:string],          :cstring, "sp_crypto_sha384_b64"
    native_func :sha384_update, [:string, :string], :cbinstr, "sp_crypto_sha384_update"
    native_func :sha384_final,  [:string],          :cbinstr, "sp_crypto_sha384_final"
    native_func :sha512_hex,    [:string],          :cstring, "sp_crypto_sha512_hex"
    native_func :sha512_bin,    [:string],          :cbinstr, "sp_crypto_sha512_bin"
    native_func :sha512_b64,    [:string],          :cstring, "sp_crypto_sha512_b64"
    native_func :sha512_update, [:string, :string], :cbinstr, "sp_crypto_sha512_update"
    native_func :sha512_final,  [:string],          :cbinstr, "sp_crypto_sha512_final"
    native_func :sha1_hex,      [:string],          :cstring, "sp_crypto_sha1_hex"
    native_func :sha1_bin,      [:string],          :cbinstr, "sp_crypto_sha1_bin"
    native_func :sha1_b64,      [:string],          :cstring, "sp_crypto_sha1_b64"
    native_func :sha1_update,   [:string, :string], :cbinstr, "sp_crypto_sha1_update"
    native_func :sha1_final,    [:string],          :cbinstr, "sp_crypto_sha1_final"
    native_func :md5_hex,       [:string],          :cstring, "sp_crypto_md5_hex"
    native_func :md5_bin,       [:string],          :cbinstr, "sp_crypto_md5_bin"
    native_func :md5_b64,       [:string],          :cstring, "sp_crypto_md5_b64"
    native_func :md5_update,    [:string, :string], :cbinstr, "sp_crypto_md5_update"
    native_func :md5_final,     [:string],          :cbinstr, "sp_crypto_md5_final"
  end

  # CRuby's Digest::Instance mixin: defines the common instance methods over
  # #update, #reset, #finish, and #block_length.
  module Instance
    def update(string)
      raise RuntimeError, "#{self.class} does not implement update()"
    end

    def <<(string) = update(string)

    def reset
      raise RuntimeError, "#{self.class} does not implement reset()"
    end

    private def finish
      raise RuntimeError, "#{self.class} does not implement finish()"
    end

    def block_length
      raise RuntimeError, "#{self.class} does not implement block_length()"
    end

    def digest_length = digest.bytesize
    alias length digest_length
    alias size digest_length

    def new = clone.reset

    def digest(string = nil)
      unless string.nil?
        reset
        update(string)
        value = finish
        reset
        value
      else
        dup.__send__(:finish)
      end
    end

    def digest!
      value = finish
      reset
      value
    end

    # unpack1 and pack answer a boxed String unless interpolated; #to_s is
    # #hexdigest, so typing it as String keeps boxed #to_s calls typed too.
    def hexdigest(string = nil) = "#{digest(string).unpack1("H*")}"
    def hexdigest! = "#{digest!.unpack1("H*")}"
    def base64digest(string = nil) = "#{[digest(string)].pack("m0")}"
    def base64digest! = "#{[digest!].pack("m0")}"

    def to_s = hexdigest
    def inspect = "#<#{self.class.name}: #{hexdigest}>"

    def ==(other)
      return hexdigest == "#{other.hexdigest}" if other.is_a?(Digest::Instance)
      return false unless other.respond_to?(:to_str)
      str = other.to_str
      str.is_a?(String) && hexdigest == str
    end

    def file(name)
      File.open(name.respond_to?(:to_path) ? name.to_path : name, "rb") do |f|
        while (chunk = f.read(16_384))
          update(chunk)
        end
      end
      self
    end
  end

  # CRuby's Digest::Class: base class for Ruby digest implementations.
  class Class
    include Instance

    def self.digest(string, *args)
      new(*args).digest(string)
    end

    def self.hexdigest(string, *args)
      "#{digest(string, *args).unpack1("H*")}"
    end

    def self.base64digest(string, *args)
      "#{[digest(string, *args)].pack("m0")}"
    end

    def self.file(name, *args)
      new(*args).file(name)
    end
  end

  # CRuby's Digest::Base: abstract base class for the C-backed built-in
  # digests. Each concrete subclass keeps only the fixed-size running state
  # String from lib/sp_crypto.c; updates produce a new state String rather than
  # mutating in place, so dup/clone keep independent state at O(1) cost.
  class Base < Digest::Class
    def initialize
      if self.class == Digest::Base
        raise NotImplementedError, "Digest::Base is an abstract class"
      end
      unless digest_algorithm
        raise RuntimeError, "Digest::Base cannot be directly inherited in Ruby"
      end
      @state = "".b
    end

    private def digest_algorithm = nil

    def update(string)
      bytes = Digest.send(:string_bytes, string)
      @state =
        case digest_algorithm
        when :sha256 then Native.sha256_update(@state, bytes)
        when :sha384 then Native.sha384_update(@state, bytes)
        when :sha512 then Native.sha512_update(@state, bytes)
        when :sha1   then Native.sha1_update(@state, bytes)
        else Native.md5_update(@state, bytes)
        end
      self
    end
    alias << update

    def reset
      @state = "".b
      self
    end

    private def finish
      case digest_algorithm
      when :sha256 then Native.sha256_final(@state)
      when :sha384 then Native.sha384_final(@state)
      when :sha512 then Native.sha512_final(@state)
      when :sha1   then Native.sha1_final(@state)
      else Native.md5_final(@state)
      end
    end

    def digest(string = nil)
      unless string.nil?
        reset
        update(string)
        value = finish
        reset
        value
      else
        finish
      end
    end

    def digest_length
      case digest_algorithm
      when :sha256 then 32
      when :sha384 then 48
      when :sha512 then 64
      when :sha1   then 20
      else 16
      end
    end

    def block_length
      algo = digest_algorithm
      (algo == :sha384 || algo == :sha512) ? 128 : 64
    end
  end

  class SHA256 < Base
    private def digest_algorithm = :sha256
    def self.hexdigest(string) = Native.sha256_hex(Digest.send(:string_bytes, string))
    def self.digest(string) = Native.sha256_bin(Digest.send(:string_bytes, string))
    def self.base64digest(string) = Native.sha256_b64(Digest.send(:string_bytes, string))
    def self.file(name) = new.file(name)
  end

  class SHA384 < Base
    private def digest_algorithm = :sha384
    def self.hexdigest(string) = Native.sha384_hex(Digest.send(:string_bytes, string))
    def self.digest(string) = Native.sha384_bin(Digest.send(:string_bytes, string))
    def self.base64digest(string) = Native.sha384_b64(Digest.send(:string_bytes, string))
    def self.file(name) = new.file(name)
  end

  class SHA512 < Base
    private def digest_algorithm = :sha512
    def self.hexdigest(string) = Native.sha512_hex(Digest.send(:string_bytes, string))
    def self.digest(string) = Native.sha512_bin(Digest.send(:string_bytes, string))
    def self.base64digest(string) = Native.sha512_b64(Digest.send(:string_bytes, string))
    def self.file(name) = new.file(name)
  end

  class SHA1 < Base
    private def digest_algorithm = :sha1
    def self.hexdigest(string) = Native.sha1_hex(Digest.send(:string_bytes, string))
    def self.digest(string) = Native.sha1_bin(Digest.send(:string_bytes, string))
    def self.base64digest(string) = Native.sha1_b64(Digest.send(:string_bytes, string))
    def self.file(name) = new.file(name)
  end

  # MD5: a legacy hash, kept because Active Storage's direct-upload protocol
  # checks a blob by the base64 MD5 of its bytes (#4631). Not for new designs.
  class MD5 < Base
    private def digest_algorithm = :md5
    def self.hexdigest(string) = Native.md5_hex(Digest.send(:string_bytes, string))
    def self.digest(string) = Native.md5_bin(Digest.send(:string_bytes, string))
    def self.base64digest(string) = Native.md5_b64(Digest.send(:string_bytes, string))
    def self.file(name) = new.file(name)
  end
end

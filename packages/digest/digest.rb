# Spinel bundled `digest` -- a native binding with NO carried C.
#
# The hash implementations are the runtime's vendored crypto (lib/sp_crypto.c,
# always in libspinel_rt.a because the string/net runtime uses it); this
# package only declares the Ruby surface. Subset: the class-method hexdigest
# and digest forms, and `.new` with `update`/`<<` (Digest::Instance below).
module Digest
  # Digest.hexencode, the module function CRuby's digest extension defines:
  # a String's bytes as lowercase hex, two digits a byte.
  def self.hexencode(string) = string.unpack1("H*")

  module SHA256
    native_lib "digest"
    def self.new = Digest::Instance.new(:sha256)
    # :cstring return: sp_crypto's static-buffer contract (the next call
    # clobbers the buffer), so codegen dups the result onto the GC heap.
    native_func :hexdigest, [:string], :cstring, "sp_crypto_sha256_hex"
    # :cbinstr return: raw digest bytes, whose length comes from
    # sp_ffi_bin_len rather than strlen (a binary digest contains NULs).
    native_func :digest, [:string], :cbinstr, "sp_crypto_sha256_bin"
    # base64digest: strict base64 of the raw digest, as Digest::Class defines
    # it for every digest.
    native_func :base64digest, [:string], :cstring, "sp_crypto_sha256_b64"
    # `.file(path)`: the digest of the file's bytes, as an object answering
    # hexdigest / digest / base64digest / to_s (Digest::FileResult below).
    def self.file(path)
      data = File.binread(path.to_s)
      Digest::FileResult.new(hexdigest(data), digest(data), base64digest(data))
    end
  end
  module SHA1
    native_lib "digest"
    def self.new = Digest::Instance.new(:sha1)
    native_func :hexdigest, [:string], :cstring, "sp_crypto_sha1_hex"
    native_func :digest, [:string], :cbinstr, "sp_crypto_sha1_bin"
    native_func :base64digest, [:string], :cstring, "sp_crypto_sha1_b64"
    # `.file(path)`: the digest of the file's bytes, as an object answering
    # hexdigest / digest / base64digest / to_s (Digest::FileResult below).
    def self.file(path)
      data = File.binread(path.to_s)
      Digest::FileResult.new(hexdigest(data), digest(data), base64digest(data))
    end
  end
  # MD5: a legacy hash, kept because Active Storage's direct-upload protocol
  # checks a blob by the base64 MD5 of its bytes (#4631). Not for new designs.
  module MD5
    native_lib "digest"
    def self.new = Digest::Instance.new(:md5)
    native_func :hexdigest, [:string], :cstring, "sp_crypto_md5_hex"
    native_func :digest, [:string], :cbinstr, "sp_crypto_md5_bin"
    native_func :base64digest, [:string], :cstring, "sp_crypto_md5_b64"
    # `.file(path)`: the digest of the file's bytes, as an object answering
    # hexdigest / digest / base64digest / to_s (Digest::FileResult below).
    def self.file(path)
      data = File.binread(path.to_s)
      Digest::FileResult.new(hexdigest(data), digest(data), base64digest(data))
    end
  end
  # What `Digest::<Algo>.new` answers. CRuby's instance keeps the running
  # hash state; this one keeps the bytes given to `update` and hashes them
  # when a digest is asked for, so the answers are the same and the memory
  # grows with the input.
  class Instance
    def initialize(algorithm)
      @algorithm = algorithm
      @data = "".b
    end

    # a copy (dup, clone) takes its own state: update appends to @data in
    # place, so a shared one changed the original too (#11348)
    def initialize_copy(other)
      @data = other.__data.dup
    end

    protected def __data = @data

    def update(string)
      @data << string.b
      self
    end

    def <<(string) = update(string)

    def reset
      @data = "".b
      self
    end

    def hexdigest
      case @algorithm
      when :sha256 then SHA256.hexdigest(@data)
      when :sha1 then SHA1.hexdigest(@data)
      else MD5.hexdigest(@data)
      end
    end

    def digest
      case @algorithm
      when :sha256 then SHA256.digest(@data)
      when :sha1 then SHA1.digest(@data)
      else MD5.digest(@data)
      end
    end

    def base64digest
      case @algorithm
      when :sha256 then SHA256.base64digest(@data)
      when :sha1 then SHA1.base64digest(@data)
      else MD5.base64digest(@data)
      end
    end
  end

  # What `Digest::<Algo>.file(path)` answers. CRuby answers an instance of
  # the digest class itself (`Digest::MD5`), which can also take further
  # `update`s; these modules have no instances, so the file's digest comes
  # back as this value, answering the read side of that instance.
  class FileResult
    def initialize(hex, bin, b64)
      @hex = hex
      @bin = bin
      @b64 = b64
    end

    def hexdigest = @hex
    def to_s = @hex
    def digest = @bin
    def base64digest = @b64
  end
end

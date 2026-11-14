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
  # The runtime's incremental hashing (lib/sp_crypto.c): a state is the
  # chaining value, the byte count and the partial block, in a binary String
  # of 104 bytes or fewer; the empty String is the initial state. Each call
  # answers a new state rather than changing the one given.
  module Native
    native_lib "digest"
    native_func :sha256_update, [:string, :string], :cbinstr, "sp_crypto_sha256_update"
    native_func :sha256_final,  [:string],          :cbinstr, "sp_crypto_sha256_final"
    native_func :sha1_update,   [:string, :string], :cbinstr, "sp_crypto_sha1_update"
    native_func :sha1_final,    [:string],          :cbinstr, "sp_crypto_sha1_final"
    native_func :md5_update,    [:string, :string], :cbinstr, "sp_crypto_md5_update"
    native_func :md5_final,     [:string],          :cbinstr, "sp_crypto_md5_final"
  end

  # What `Digest::<Algo>.new` answers. It keeps the running hash state, as
  # CRuby's instance does: an update hashes the whole blocks it completes
  # and keeps the rest, so the memory stays the same however much is fed,
  # and a digest hashes only the last block or two. A copy (dup, clone)
  # shares the state String, which no update changes in place (#11348), so
  # a copy of an object that has hashed a key's block starts from there.
  class Instance
    def initialize(algorithm)
      @algorithm = algorithm
      @state = "".b
    end

    def update(string)
      @state =
        case @algorithm
        when :sha256 then Native.sha256_update(@state, string.b)
        when :sha1 then Native.sha1_update(@state, string.b)
        else Native.md5_update(@state, string.b)
        end
      self
    end

    def <<(string) = update(string)

    def reset
      @state = "".b
      self
    end

    def digest
      case @algorithm
      when :sha256 then Native.sha256_final(@state)
      when :sha1 then Native.sha1_final(@state)
      else Native.md5_final(@state)
      end
    end

    # unpack1 and pack answer a String the interpolation types
    def hexdigest = "#{digest.unpack1("H*")}"

    def base64digest = "#{[digest].pack("m0")}"
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

# Spinel bundled `openssl`.
#
# The spelling is CRuby's, deliberately: the goal is that a program written
# against CRuby compiles here, so inventing a shorter name would be worth
# nothing. What is missing is missing the way a subset is missing things --
# most of X509 is not here, and a program that uses it fails at COMPILE time
# with an unresolved call rather than at run time somewhere deep. Digest,
# HMAC, KDF, PKey::EC and Cipher are here, and the last two are subsets of
# CRuby's in a second sense, each spelled out in its own file: EC keeps
# CRuby's method names but trades BN, EC::Group and EC::Point for the raw
# bytes every protocol that names an EC key means by them, and Cipher keeps
# CRuby's object while buffering rather than streaming, over aes-128-gcm and
# aes-256-gcm only.
#
# Spinel implements neither TLS nor any of the arithmetic below it.
# sp_openssl.c is glue over the system libssl and libcrypto, and the trust
# anchors are the operating system's; nothing is bundled.
#
# SSLSocket is not an IO, exactly as in CRuby: it is an ordinary object that
# answers #to_io, which is why IO.select had to learn that protocol. The
# handle is an Integer naming a connection in the C table -- no SSL pointer
# is handed to a garbage-collected world.

# CRuby's openssl extension requires digest, so a program that requires only
# "openssl" can name the top-level Digest.
require "digest"
require "openssl/buffering"
require "openssl/digest"
require "openssl/cipher"
require "openssl/kdf"
require "openssl/pkey"

module OpenSSL
  module Native
    native_lib "openssl"
    native_obj "packages/openssl/sp_openssl.o"
    native_func :connect,      [:int, :string, :int, :any], :int,    "sp_ssl_connect"
    native_func :read,         [:int, :int],          :string, "sp_ssl_read"
    native_func :write,        [:int, :string, :int], :int,    "sp_ssl_write"
    native_func :pending,      [:int],                :int,    "sp_ssl_pending"
    native_func :close,        [:int],                :int,    "sp_ssl_close"
    native_func :last_error,   [],                    :string, "sp_ssl_last_error"
    native_func :peer_subject, [:int],                :string, "sp_ssl_peer_subject"
    native_func :version,      [:int],                :string, "sp_ssl_version"
    native_func :cipher,       [:int],                :string, "sp_ssl_cipher"
    native_func :connect_nb,   [:int, :string, :int, :any], :int,    "sp_ssl_connect_nb"
    native_func :connect_cont, [:int],                :int,    "sp_ssl_connect_cont"
    native_func :read_nb,      [:int, :int],          :string, "sp_ssl_read_nb"
    native_func :want,         [],                    :int,    "sp_ssl_want"
    native_func :write_nb,     [:int, :string, :int], :int,    "sp_ssl_write_nb"

    # EC, over the same object. Bytes in, bytes out, and an empty answer
    # means the reason is in last_error -- see openssl/pkey.rb.
    native_func :ec_generate,     [:string],                   :cbinstr, "sp_ec_generate"
    native_func :ec_public_bytes, [:string, :string],          :cbinstr, "sp_ec_public_bytes"
    native_func :ec_dh,           [:string, :string, :string], :cbinstr, "sp_ec_dh"

    # AES-GCM, one shot. The whole operation in one call, because the Ruby
    # Cipher over it holds the state -- see openssl/cipher.rb. Encrypt answers
    # ciphertext || tag; both answer "" with the reason in last_error, since an
    # empty result is legitimate here (the plaintext of an empty message).
    native_func :aes_gcm_encrypt, [:string, :string, :string, :string, :string],
                :string, "sp_aes_gcm_encrypt"
    native_func :aes_gcm_decrypt, [:string, :string, :string, :string, :string, :string],
                :string, "sp_aes_gcm_decrypt"

    # ECDSA over the same curves, with signatures as raw r || s rather than
    # DER -- the conversion is in the C because a DER signature is a
    # well-formed String that every JWT verifier rejects.
    native_func :ecdsa_sign,    [:string, :string, :string],          :cbinstr, "sp_ecdsa_sign"
    native_func :ecdsa_verify,  [:string, :string, :string, :string], :int,     "sp_ecdsa_verify"
    native_func :ec_check_point, [:string, :string],                  :int,     "sp_ec_check_point"

    ffi_lib "ssl"
    ffi_lib "crypto"

    # X509::Store - for system CA cert loading
    native_struct "X509::Store", "sp_X509_Store", "sp_X509_Store_free"
    native_new [], "sp_X509_Store_new"
    native_method :set_default_paths, [], :int, "sp_X509_Store_set_default_paths"
  end
  module SSL
    VERIFY_NONE = 0
    VERIFY_PEER = 1

    class SSLError < OpenSSLError
    end

    # CRuby raises these, not an SSLError extended with a module at run time.
    # A non-blocking TLS read can need the socket to become WRITABLE before it
    # can read -- a renegotiating peer does that -- so the two directions are
    # separate classes and the caller waits on the right one:
    #
    #   begin
    #     data = ssl.read_nonblock(4096)
    #   rescue IO::WaitReadable
    #     IO.select([ssl]); retry
    #   rescue IO::WaitWritable
    #     IO.select(nil, [ssl]); retry
    #   end
    class SSLErrorWaitReadable < SSLError
      include IO::WaitReadable
    end

    class SSLErrorWaitWritable < SSLError
      include IO::WaitWritable
    end

    # Only the members an outbound HTTPS client reaches. set_params is the one
    # Net::HTTP calls; the rest of CRuby's forty-odd accessors are not here.
    class SSLContext
      attr_accessor :verify_mode
      attr_accessor :verify_hostname
      attr_accessor :cert_store

      def initialize
        @verify_mode = VERIFY_PEER
        @verify_hostname = true
        @cert_store = nil
      end

      def set_params(params = nil)
        @verify_mode = VERIFY_PEER
        @verify_hostname = true
        self
      end
    end

    class SSLSocket
      # Not an IO: an ordinary object with the buffered surface mixed in and
      # the handle behind #to_io, exactly as CRuby has it.
      include OpenSSL::Buffering

      attr_accessor :hostname
      # sysclose takes the socket with it when this is set, which is what
      # lets a caller hand the socket over and stop tracking it. CRuby's
      # unset default reads back nil where this one reads false: a statically
      # typed ivar has no third state to start in.
      attr_accessor :sync_close

      def initialize(io, context = nil)
        super()
        @io = io
        @context = context.nil? ? SSLContext.new : context
        @handle = -1
        @hostname = ""
        @sync_close = false
      end

      def context
        @context
      end

      # CRuby's SSLSocket#to_io answers the underlying socket, and every
      # forwarder (fileno, addr, closed?) goes through it. IO.select uses it
      # to find the descriptor to wait on.
      def to_io
        @io
      end

      def connect
        h = Native.connect(@io.fileno, @hostname,
                           @context.verify_mode == VERIFY_NONE ? 0 : 1,
                           @context.cert_store)
        if h < 0
          raise SSLError, "SSL_connect returned an error: #{Native.last_error}"
        end
        @handle = h
        self
      end

      # Non-blocking connect: answers the SSLSocket once the handshake is
      # done, or :wait_readable / :wait_writable when it needs the descriptor
      # in that state, which is CRuby's contract for
      # connect_nonblock(exception: false).
      #
      # Call it again to resume. The first call builds the connection and
      # takes one step; every call after that takes one more, which is why the
      # handle is kept even though the handshake is unfinished -- there is
      # nowhere else to hold OpenSSL's half-done state.
      #
      # Unlike sysread_nonblock this DOES set O_NONBLOCK on the descriptor.
      # The handshake is the first thing on a connection, so there is no
      # earlier call to have set it, and leaving it blocking would put the
      # wait back in the kernel, which is the one thing this call exists to
      # avoid.
      def connect_nonblock(exception: true)
        if @handle < 0
          h = Native.connect_nb(@io.fileno, @hostname,
                                @context.verify_mode == VERIFY_NONE ? 0 : 1,
                                @context.cert_store)
          if h < 0
            raise SSLError, "SSL_connect returned an error: #{Native.last_error}"
          end
          @handle = h
        else
          Native.connect_cont(@handle)
        end
        case Native.want
        when 0
          self
        when 1
          return :wait_readable unless exception
          raise SSLErrorWaitReadable, "read would block"
        when 2
          return :wait_writable unless exception
          raise SSLErrorWaitWritable, "write would block"
        else
          raise SSLError, "SSL_connect returned an error: #{Native.last_error}"
        end
      end

      def sysread(maxlen)
        raise SSLError, "not connected" if @handle < 0
        s = Native.read(@handle, maxlen)
        s.empty? ? nil : s
      end

      def syswrite(data)
        raise SSLError, "not connected" if @handle < 0
        n = Native.write(@handle, data, data.bytesize)
        raise SSLError, "SSL_write returned an error: #{Native.last_error}" if n < 0
        n
      end

      # One record-layer read that never blocks. The descriptor must already
      # be non-blocking -- CRuby says the same -- and this does not set it,
      # because the fd belongs to the IO the caller handed over.
      #
      # `exception: false` answers :wait_readable / :wait_writable instead of
      # raising, and nil at EOF, which is CRuby's contract for the same call.
      def sysread_nonblock(maxlen, exception: true)
        raise SSLError, "not connected" if @handle < 0
        return "" if maxlen == 0
        s = Native.read_nb(@handle, maxlen)
        return s unless s.empty?
        case Native.want
        when 1
          return :wait_readable unless exception
          raise SSLErrorWaitReadable, "read would block"
        when 2
          return :wait_writable unless exception
          raise SSLErrorWaitWritable, "write would block"
        when 3
          return nil unless exception
          raise EOFError, "end of file reached"
        else
          raise SSLError, "SSL_read returned an error: #{Native.last_error}"
        end
      end

      # The mirror of sysread_nonblock. A TLS write can need the socket to
      # become READABLE before it can write -- the peer's side of a
      # renegotiation -- so this raises the other class too, and a caller that
      # waits on the wrong direction waits forever.
      def syswrite_nonblock(data, exception: true)
        raise SSLError, "not connected" if @handle < 0
        s = data.to_s
        return 0 if s.empty?
        n = Native.write_nb(@handle, s, s.bytesize)
        return n if n > 0
        case Native.want
        when 1
          return :wait_readable unless exception
          raise SSLErrorWaitReadable, "read would block"
        when 2
          return :wait_writable unless exception
          raise SSLErrorWaitWritable, "write would block"
        when 3
          return nil unless exception
          raise EOFError, "end of file reached"
        else
          raise SSLError, "SSL_write returned an error: #{Native.last_error}"
        end
      end

      # Bytes already decrypted and waiting inside the record layer. An event
      # loop that waits on the descriptor alone will not see these: a whole
      # record can arrive in one read, leaving the fd quiet while the
      # application still has data to take. CRuby has the same trap and the
      # same escape hatch.
      def pending
        @handle < 0 ? 0 : Native.pending(@handle)
      end

      def sysclose
        return nil if @handle < 0
        Native.close(@handle)
        @handle = -1
        @io.close if @sync_close && @io && !@io.closed?
        nil
      end

      def peer_subject
        @handle < 0 ? "" : Native.peer_subject(@handle)
      end

      def ssl_version
        @handle < 0 ? "" : Native.version(@handle)
      end

      def cipher_name
        @handle < 0 ? "" : Native.cipher(@handle)
      end
    end
  end
end

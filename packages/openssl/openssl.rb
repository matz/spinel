# Spinel bundled `openssl`.
#
# The spelling is CRuby's, deliberately: the goal is that a program written
# against CRuby compiles here, so inventing a shorter name would be worth
# nothing. What is missing is missing the way a subset is missing things --
# most of X509 is not here, and a program that uses it fails at COMPILE time
# with an unresolved call rather than at run time somewhere deep. Digest,
# HMAC, KDF, PKey::EC, Cipher and X509::Certificate are here; a certificate
# is its DER, read from PEM or DER and written back, and its fields are not
# read (openssl/x509.rb). PKey::EC and Cipher are subsets of
# CRuby's in a second sense, each spelled out in its own file: EC keeps
# CRuby's method names but trades BN, EC::Group and EC::Point for the raw
# bytes every protocol that names an EC key means by them, and Cipher keeps
# CRuby's object while buffering rather than streaming, over aes-128-gcm and
# aes-256-gcm only.
#
# Spinel implements neither TLS nor any of the arithmetic below it.
# sp_openssl.c is glue over the system libssl and libcrypto, and the trust
# anchors are the operating system's, or the ones a program adds to an
# X509::Store of its own; nothing is bundled.
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
require "openssl/x509"
require "openssl/random"

module OpenSSL
  def self.fixed_length_secure_compare(a, b)
    a = comparison_string(a)
    b = comparison_string(b)
    Native.fixed_length_secure_compare(a, b) == 0
  end

  def self.secure_compare(a, b)
    hashed_a = Digest.digest("SHA256", comparison_string(a))
    hashed_b = Digest.digest("SHA256", comparison_string(b))
    fixed_length_secure_compare(hashed_a, hashed_b) && a == b
  end

  def self.comparison_string(value)
    case value
    when String then return value
    when nil, true, false then raise TypeError, "no implicit conversion of #{value.inspect} into String"
    end
    if value.respond_to?(:to_str, true)
      converted = value.__send__(:to_str)
      return converted if String === converted
      raise TypeError, "can't convert #{value.class} to String (#{value.class}#to_str gives #{converted.class})"
    end
    raise TypeError, "no implicit conversion of #{value.class} into String"
  end
  private_class_method :comparison_string

  module Native
    native_lib "openssl"
    native_obj "packages/openssl/sp_openssl.o"
    native_func :fixed_length_secure_compare, [:string, :string], :int, "sp_openssl_fixed_length_secure_compare"
    native_func :pending,      [:int],                :int,    "sp_ssl_pending"
    native_func :close,        [:int],                :int,    "sp_ssl_close"
    native_func :discard,      [:int],                :int,    "sp_ssl_discard"
    native_func :last_error,   [],                    :string, "sp_ssl_last_error"
    native_func :peer_subject, [:int],                :string, "sp_ssl_peer_subject"
    native_func :version,      [:int],                :string, "sp_ssl_version"
    native_func :cipher,       [:int],                :string, "sp_ssl_cipher"
    native_func :connect_nb,   [:int, :string, :int, :any, :string, :string, :string], :int, "sp_ssl_connect_nb"
    native_func :handshake_cont, [:int],              :int,    "sp_ssl_handshake_cont"
    native_func :read_nb,      [:int, :int],          :string, "sp_ssl_read_nb"
    native_func :want,         [],                    :int,    "sp_ssl_want"
    native_func :write_nb,     [:int, :string, :int], :int,    "sp_ssl_write_nb"
    # A context's own certificates as x509_load lays them out, the leaf
    # first, and its key as its curve and scalar: a server's, and a client's
    # for a server that asks. See SSLContext below.
    native_func :accept_nb,    [:int, :string, :string, :string, :int, :any], :int, "sp_ssl_accept_nb"
    native_func :server_check, [:string, :string, :string],       :int, "sp_ssl_server_check"

    # EC, over the same object. Bytes in, bytes out, and an empty answer
    # means the reason is in last_error -- see openssl/pkey.rb.
    native_func :ec_generate,     [:string],                   :cbinstr, "sp_ec_generate"
    native_func :ec_public_bytes, [:string, :string],          :cbinstr, "sp_ec_public_bytes"
    native_func :ec_dh,           [:string, :string, :string], :cbinstr, "sp_ec_dh"

    # PKey.read: an EC or RSA key out of PEM or DER, as the half it carries
    # and the key's bytes. See openssl/pkey.rb.
    native_func :pkey_read,       [:string],                   :cbinstr, "sp_pkey_read"

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

    # RSA verify over a public key given as its modulus and exponent bytes:
    # n, e, digest, data, signature, then PSS (1) or PKCS#1 v1.5 (0), and for
    # PSS the MGF1 digest and the salt length. See openssl/pkey.rb.
    native_func :rsa_verify, [:string, :string, :string, :string, :string, :int, :string, :int],
                :int, "sp_rsa_verify"
    # And signing, with a private key as its PKCS#1 DER: the DER, digest,
    # data, then PSS (1) or PKCS#1 v1.5 (0), the MGF1 digest and the salt
    # length. "" with the reason in last_error.
    native_func :rsa_sign, [:string, :string, :string, :int, :string, :int],
                :cbinstr, "sp_rsa_sign"
    # A public key's to_der: n and e in, the SubjectPublicKeyInfo DER out.
    native_func :rsa_public_der, [:string, :string], :cbinstr, "sp_rsa_public_der"

    # X509::Certificate: PEM or DER in, DER out, and "" with the reason in
    # last_error. x509_load answers each certificate as a 4-byte big-endian
    # length and its DER. See openssl/x509.rb.
    native_func :x509_der,  [:string], :cbinstr, "sp_x509_der"
    native_func :x509_load, [:string], :cbinstr, "sp_x509_load"
    native_func :x509_pem,  [:string], :cbinstr, "sp_x509_pem"
    # X509::Store#add_cert: the store, and the certificate's DER.
    native_func :x509_store_add, [:any, :string], :int, "sp_x509_store_add"

    ffi_lib "ssl"
    ffi_lib "crypto"

    # X509::Store - for system CA cert loading, or a CA of the program's own.
    # Declared under its whole path, which is the name it answers to and the
    # one the reopening at the end of this file spells.
    native_struct "OpenSSL::X509::Store", "sp_X509_Store", "sp_X509_Store_free"
    native_new [], "sp_X509_Store_new"
    native_method :set_default_paths, [], :int, "sp_X509_Store_set_default_paths"
  end
  module SSL
    VERIFY_NONE = 0
    VERIFY_PEER = 1
    VERIFY_FAIL_IF_NO_PEER_CERT = 2
    VERIFY_CLIENT_ONCE = 4

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

    # The members an outbound HTTPS client reaches, and the certificate and
    # key a server presents. set_params is the one Net::HTTP calls; the rest
    # of CRuby's forty-odd accessors are not here.
    #
    # A context holds its certificates and key as the objects it was given,
    # and each accept or connect builds OpenSSL's context from their bytes
    # (#9341): there is no setup step, and nothing to free. A client sends
    # them to a server that asks for a certificate.
    #
    # verify_mode reads VERIFY_PEER until it is set, where CRuby's reads nil,
    # and a server asks for a client certificate only once the program has
    # set it (4a in #9341): a default context here would otherwise ask every
    # client for one, which CRuby's does not.
    class SSLContext
      attr_reader :verify_mode
      attr_accessor :verify_hostname
      attr_accessor :cert_store
      attr_accessor :cert
      attr_accessor :key
      attr_accessor :extra_chain_cert

      def initialize
        @verify_mode = VERIFY_PEER
        @verify_hostname = true
        @cert_store = nil
        @cert = nil
        @key = nil
        @extra_chain_cert = nil
        @added_chain = ""
        @added_key = nil
        @verify_mode_set = false
      end

      def verify_mode=(mode)
        @verify_mode = mode
        @verify_mode_set = true
      end

      # CRuby's defaults, VERIFY_PEER and verify_hostname, with what params
      # gives for those two, set through the writer as CRuby's set_params
      # sets them: Net::SMTP's tls_verify: false passes VERIFY_NONE here, and
      # a server context asks for a client certificate unless params says
      # VERIFY_NONE. The other keys CRuby's takes name members this context
      # does not have, and are not read. CRuby answers the merged params;
      # this answers the context.
      def set_params(params = nil)
        mode = VERIFY_PEER
        hostname = true
        unless params.nil?
          mode = params[:verify_mode].to_i if params.key?(:verify_mode)
          hostname = params[:verify_hostname] ? true : false if params.key?(:verify_hostname)
        end
        self.verify_mode = mode
        @verify_hostname = hostname
        self
      end

      # Not CRuby's: the verify_mode SSLSocket#accept hands to the C, which is
      # VERIFY_NONE until the program sets one.
      def accept_verify_mode
        @verify_mode_set ? @verify_mode : VERIFY_NONE
      end

      # CRuby's, and checked when it is called, as CRuby checks it: a key
      # without its private half, or one that is not the certificate's, raises
      # here rather than at the first accept. It leaves cert, key and
      # extra_chain_cert as they were, as in CRuby. The key is a PKey::EC or
      # a PKey::RSA. A second call replaces the first, where CRuby keeps one
      # certificate per kind of key, an EC one and an RSA one side by side.
      def add_certificate(certificate, pkey, extra_certs = nil)
        raise ArgumentError, "private key is needed" unless pkey.private?
        chain = SSLContext.chain_bytes(certificate, extra_certs)
        if Native.server_check(chain, SSLContext.key_curve(pkey), SSLContext.key_bytes(pkey)) != 1
          raise ArgumentError, "public key mismatch"
        end
        @added_chain = chain
        @added_key = pkey
        self
      end

      # Not CRuby's: what SSLSocket#accept and #connect hand to the C. cert=
      # and key= win over add_certificate when both are set, as in CRuby,
      # whose setup applies them after it; extra_chain_cert is sent with
      # either. With neither, all three are empty: a server's handshake fails
      # as CRuby's does without a certificate, and a client sends none.
      def own_chain
        if @cert && @key
          SSLContext.chain_bytes(@cert, @extra_chain_cert)
        elsif @added_key
          @added_chain + SSLContext.chain_bytes(nil, @extra_chain_cert)
        else
          ""
        end
      end

      def own_key
        @cert && @key ? @key : @added_key
      end

      # Not CRuby's: a private key as the C takes it, an EC key as its curve
      # and scalar, an RSA key as an empty curve and its PKCS#1 DER.
      def self.key_curve(key)
        key.is_a?(PKey::EC) ? key.curve : ""
      end

      def self.key_bytes(key)
        key.is_a?(PKey::EC) ? key.private_key_bytes : key.to_der
      end

      # Each certificate as a 4-byte big-endian length and its DER, the leaf
      # first: the layout Native.x509_load answers in.
      def self.chain_bytes(leaf, extra)
        out = "".b
        out << [leaf.to_der.bytesize].pack("N") << leaf.to_der if leaf
        (extra || []).each { |c| out << [c.to_der.bytesize].pack("N") << c.to_der }
        out
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

      # CRuby forwards the socket options to the underlying socket
      # (OpenSSL::SSL::SocketForwarder)
      def setsockopt(level, optname, optval)
        @io.setsockopt(level, optname, optval)
        0
      end

      # Blocking, as CRuby's is, and driven the way CRuby drives it: one
      # handshake step at a time over a non-blocking descriptor, waiting on
      # the descriptor in between. The wait is IO.select's, which parks a
      # green thread and hands its OS worker back; a wait inside SSL_connect
      # would keep the worker in the kernel for as long as the peer is quiet,
      # and every other thread stopped at the next collection with it.
      def connect
        start_connect
        finish_handshake("SSL_connect")
      end

      # The server's #connect: the handshake on an accepted socket, with the
      # context's certificate and key, waiting the same way. A client that
      # connects and never sends its hello parks this thread, not a worker.
      def accept
        start_accept
        finish_handshake("SSL_accept")
      end

      # The server's #connect_nonblock, with the same answers and the same
      # resumption: the first call builds the connection and takes one step,
      # and each call after it takes one more.
      def accept_nonblock(exception: true)
        if @handle < 0
          start_accept
        else
          Native.handshake_cont(@handle)
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
          # The slot goes back here, not at sysclose: a server that rescues
          # a failed handshake closes the TCP socket and drops this object,
          # and on a public port failed handshakes are routine.
          reason = Native.last_error
          Native.discard(@handle)
          @handle = -1
          raise SSLError, "SSL_accept returned an error: #{reason}"
        end
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
          start_connect
        else
          Native.handshake_cont(@handle)
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

      # The blocking pair waits the way #connect does: on the descriptor,
      # between non-blocking record-layer calls, so a peer that sends nothing
      # parks this thread rather than holding a worker in SSL_read. The
      # answers are what they were: nil at EOF (and on a failed read), the
      # byte count from a write, SSLError when a write fails.
      def sysread(maxlen)
        raise SSLError, "not connected" if @handle < 0
        loop do
          s = Native.read_nb(@handle, maxlen)
          return s unless s.empty?
          want = Native.want
          if want == 1
            IO.select([@io])
          elsif want == 2
            IO.select(nil, [@io])
          else
            return nil
          end
        end
      end

      def syswrite(data)
        raise SSLError, "not connected" if @handle < 0
        return 0 if data.bytesize == 0
        loop do
          n = Native.write_nb(@handle, data, data.bytesize)
          return n if n > 0
          want = Native.want
          if want == 1
            IO.select([@io])
          elsif want == 2
            IO.select(nil, [@io])
          else
            raise SSLError, "SSL_write returned an error: #{Native.last_error}"
          end
        end
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

      # The socket goes with it under sync_close whether or not a handshake
      # ever finished, as in CRuby: SSLServer#accept closes a failed one
      # through this.
      def sysclose
        if @handle >= 0
          Native.close(@handle)
          @handle = -1
        end
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

      private

      # The client's certificate and key go with every connect, and OpenSSL
      # sends them only to a server that asks.
      def start_connect
        key = own_key
        h = Native.connect_nb(@io.fileno, @hostname,
                              @context.verify_mode == VERIFY_NONE ? 0 : 1,
                              @context.cert_store, @context.own_chain,
                              key ? SSLContext.key_curve(key) : "", key ? SSLContext.key_bytes(key) : "")
        if h < 0
          raise SSLError, "SSL_connect returned an error: #{Native.last_error}"
        end
        @handle = h
      end

      def start_accept
        key = own_key
        h = Native.accept_nb(@io.fileno, @context.own_chain,
                             key ? SSLContext.key_curve(key) : "", key ? SSLContext.key_bytes(key) : "",
                             @context.accept_verify_mode, @context.cert_store)
        if h < 0
          raise SSLError, "SSL_accept returned an error: #{Native.last_error}"
        end
        @handle = h
      end

      # A key without its private half raises here, CRuby's ArgumentError,
      # which CRuby raises at SSLSocket.new; a key that is not the
      # certificate's is the C's SSLError, as CRuby's is.
      def own_key
        key = @context.own_key
        raise ArgumentError, "private key is needed" if key && !key.private?
        key
      end

      # Both blocking handshakes, after their first step: wait on the
      # descriptor in the direction the last step asked for, then take the
      # next. A failure gives the slot back before it raises.
      def finish_handshake(what)
        # want is per OS thread, so it is read straight after the call that
        # set it: a loop back-edge is a preemption point, and a thread can
        # resume on another worker.
        want = Native.want
        until want == 0
          if want == 1
            IO.select([@io])
          elsif want == 2
            IO.select(nil, [@io])
          else
            reason = Native.last_error
            Native.discard(@handle)
            @handle = -1
            raise SSLError, "#{what} returned an error: #{reason}"
          end
          Native.handshake_cont(@handle)
          want = Native.want
        end
        self
      end
    end

    # CRuby's SSLServer: a TCPServer whose accept answers an SSLSocket. The
    # handshake is made inside accept, as in CRuby, so a client that
    # connects and sends nothing holds the accept until it does. A server on
    # a public port sets start_immediately to false and bounds each
    # handshake itself, with SSLSocket#accept_nonblock and IO.select.
    # CRuby also sets the context's session_id_context here; a session does
    # not resume across two accepts in this package, so there is none.
    class SSLServer
      attr_accessor :start_immediately

      def initialize(svr, ctx)
        @svr = svr
        @ctx = ctx
        @start_immediately = true
      end

      def to_io
        @svr
      end

      def listen(backlog = Socket::SOMAXCONN)
        @svr.listen(backlog)
      end

      def shutdown(how = Socket::SHUT_RDWR)
        @svr.shutdown(how)
      end

      # The SSLSocket takes the accepted socket with it when it closes, and
      # a handshake that fails closes both before it raises.
      def accept
        sock = @svr.accept
        ssl = SSLSocket.new(sock, @ctx)
        ssl.sync_close = true
        begin
          ssl.accept if @start_immediately
        rescue Exception => e
          ssl.close
          raise e
        end
        ssl
      end

      def close
        @svr.close
      end

      # CRuby's SocketForwarder, for the members a server reads.
      def addr
        @svr.addr
      end

      def fileno
        @svr.fileno
      end

      def closed?
        @svr.closed?
      end
    end
  end
end

# Reopened at the top level, under the path it was declared with: that is
# what makes it the native class rather than a second Store.
class OpenSSL::X509::Store
  # CRuby's, for a client that trusts a CA of its own rather than, or as well
  # as, the operating system's. The store keeps its own copy, so a
  # certificate dropped after this stays trusted.
  def add_cert(cert)
    if OpenSSL::Native.x509_store_add(self, cert.to_der) != 1
      raise OpenSSL::X509::StoreError, OpenSSL::Native.last_error
    end
    self
  end
end

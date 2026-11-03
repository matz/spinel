# Spinel bundled `net/http` -- an HTTP/HTTPS client.
#
# The spelling is CRuby's, so a program written against it compiles here:
#
#     Net::HTTP.get(URI("https://example.com/"))
#     res = Net::HTTP.get_response(URI("https://example.com/"))
#     Net::HTTP.start(host, 443, use_ssl: true) { |http| http.request(req) }
#
# Pure Ruby over TCPSocket and the openssl package: nothing here speaks TLS,
# it asks OpenSSL::SSL::SSLSocket for a connection and then reads and writes
# lines. Which also means an https request needs the openssl package, and says
# so if it is missing rather than failing somewhere lower down.
#
# What is absent is absent the way a subset is, and a program naming it fails
# to compile rather than at run time:
#
# * HTTP/1.1. The requests of one `start` session share its connection while
#   both sides keep it alive (keep_alive_timeout, max_retries); a request
#   with no session asks for `Connection: close`. There is no pipelining, no
#   connection pool shared between sessions and no HTTP/2
# * no proxy support, no cookie jar, no automatic redirect following
#   (#get_response hands back the 3xx and its Location, as CRuby's does)
# * a gzip or deflate body is inflated whole once it is read; not streamed
require "socket"
require "timeout"
require "net/protocol"
require "uri"
require "zlib"

module Net
  # The part of CRuby's protocol error tree the HTTP errors sit under, so a
  # `rescue Net::ProtocolError` catches what it catches there.
  class ProtocolError < StandardError; end
  class ProtoFatalError < ProtocolError; end
  class ProtoServerError < ProtocolError; end
  class ProtoRetriableError < ProtocolError; end

  # A status line or a header line that is not one, as CRuby raises them.
  # Neither is a ProtocolError there, so a `rescue Net::ProtocolError` does
  # not catch them here either.
  class HTTPBadResponse < StandardError; end
  class HTTPHeaderSyntaxError < StandardError; end

  # The response an HTTP error came from, as CRuby carries it. The package
  # also raises HTTPError itself with a message alone (no status line at
  # all), so the response is optional here.
  module HTTPExceptions
    def initialize(msg, res = nil)
      super(msg)
      @response = res
    end

    attr_reader :response
  end

  class HTTPError < ProtocolError
    include HTTPExceptions
  end

  class HTTPRetriableError < ProtoRetriableError
    include HTTPExceptions
  end

  class HTTPClientException < ProtoServerError
    include HTTPExceptions
  end

  class HTTPFatalError < ProtoFatalError
    include HTTPExceptions
  end

  # Raised when the peer accepts the connection and then says nothing for
  # read_timeout seconds, and when a connection cannot be established within
  # open_timeout. Same classes and same base as CRuby's: Timeout::Error, so
  # `rescue Timeout::Error` catches either, which is what a caller with a
  # deadline writes.
  class ReadTimeout < Timeout::Error
    def message = "Net::ReadTimeout"
  end

  class OpenTimeout < Timeout::Error
    def message = "Net::OpenTimeout"
  end

  # The response. `code` is a String, as CRuby has it ("200", not 200), and
  # header lookup through #[] is case-insensitive.
  #
  # A header the server sent more than once (Set-Cookie is the usual one)
  # reads through #[] as its values joined with ", ", as CRuby's does, and
  # through #get_fields as the separate values. `fields` holds those; a
  # response built without it has one value per header.
  class HTTPResponse
    attr_reader :http_version, :code, :message

    def initialize(http_version, code, message, headers, body, fields = nil)
      @http_version = http_version
      @code = code
      @message = message
      @headers = headers          # downcased name => value
      @body = body
      @body_reader = nil
      @decode_content = false
      @read = false
      @fields = {}
      if fields.nil?
        headers.each { |k, v| @fields[k] = [v] }
      else
        fields.each { |k, v| @fields[k] = v }
      end
    end

    def reading_body(reader, decode_content)
      @body_reader = reader
      @decode_content = decode_content
    end

    def body
      read_body
    end

    def read_body(dest = nil, &block)
      if @read
        raise IOError, "#{self.class}#read_body called twice" if dest || block
        return @body
      end
      raise ArgumentError, "both arg and block given for HTTP method" if dest && block
      if !@body_reader.nil? || (!@body.nil? && (dest || block))
        out = block.nil? ? (dest || String.new) : ReadAdapter.new(block)
        if @body_reader.nil?
          out << @body
        else
          @body_reader.read_entity_body(@headers, @fields, out, @decode_content)
        end
        @body = out
      end
      @body_reader = nil
      @read = true
      @body
    end

    def [](name)
      @headers[name.to_s.downcase]
    end

    # Each value of the header, in the order they came, or nil when it is
    # absent. A copy, as CRuby's is.
    def get_fields(name)
      values = @fields[name.to_s.downcase]
      values.nil? ? nil : values.map { |v| v.dup }
    end

    def key?(name)
      @headers.key?(name.to_s.downcase)
    end

    def each_header
      @headers.each { |k, v| yield k, v }
      nil
    end

    # The media type alone, as CRuby answers it: `text/html; charset=utf-8`
    # reads `text/html`. The whole header made a type check written against
    # CRuby (`res.content_type == "text/html"`) fail for nearly every real
    # server, which sends the charset. Case is kept, as CRuby keeps it, and
    # so is CRuby's main_type/sub_type split: a third `/` part is dropped and
    # an empty subtype leaves the main type alone (`text/` reads `text`).
    def content_type
      v = @headers["content-type"]
      return nil if v.nil?
      parts = v.split(";", 2)[0].to_s.split("/")
      main = parts[0].to_s.strip
      return main if parts.length < 2
      main + "/" + parts[1].to_s.strip
    end

    # Raises unless the response is a 2xx, as CRuby's does. The message is the
    # code and the quoted reason (`404 "Not Found"`), the class comes from the
    # family (error_type below) and the exception carries the response.
    def value
      error! unless is_a?(HTTPSuccess)
    end

    def error!
      message = @code
      message = "#{message} #{@message.dump}" unless @message.nil?
      raise error_type.new(message, self)
    end

    # CRuby's EXCEPTION_TYPE of the family: a 3xx is retriable, a 4xx the
    # client's error, a 5xx fatal, and anything else a plain HTTPError.
    def error_type
      HTTPError
    end
  end

  # The response family. CRuby's success test is `res.is_a?(Net::HTTPSuccess)`
  # rather than a predicate on the code, so the classes have to exist for that
  # line to compile. The families are all here; of the per-code classes only
  # the ones a client actually names are, which is the usual subset rule:
  # the common ones, and those an API client branches on (202, 304, the
  # 307/308 redirects, 405, 409, 410, 422, 429, 502, 503, 504).
  class HTTPInformation < HTTPResponse; end
  class HTTPSuccess < HTTPResponse; end

  class HTTPRedirection < HTTPResponse
    def error_type = HTTPRetriableError
  end

  class HTTPClientError < HTTPResponse
    def error_type = HTTPClientException
  end

  class HTTPServerError < HTTPResponse
    def error_type = HTTPFatalError
  end

  class HTTPOK < HTTPSuccess; end
  class HTTPCreated < HTTPSuccess; end
  class HTTPAccepted < HTTPSuccess; end
  class HTTPNoContent < HTTPSuccess; end
  class HTTPMovedPermanently < HTTPRedirection; end
  class HTTPFound < HTTPRedirection; end
  class HTTPNotModified < HTTPRedirection; end
  class HTTPTemporaryRedirect < HTTPRedirection; end
  class HTTPPermanentRedirect < HTTPRedirection; end
  class HTTPBadRequest < HTTPClientError; end
  class HTTPUnauthorized < HTTPClientError; end
  class HTTPForbidden < HTTPClientError; end
  class HTTPNotFound < HTTPClientError; end
  class HTTPMethodNotAllowed < HTTPClientError; end
  class HTTPConflict < HTTPClientError; end
  class HTTPGone < HTTPClientError; end
  class HTTPUnprocessableEntity < HTTPClientError; end
  class HTTPTooManyRequests < HTTPClientError; end
  class HTTPInternalServerError < HTTPServerError; end
  class HTTPBadGateway < HTTPServerError; end
  class HTTPServiceUnavailable < HTTPServerError; end
  class HTTPGatewayTimeout < HTTPServerError; end

  # A request. CRuby builds these as Net::HTTP::Get.new(path) and friends;
  # the same shape is here so the same code compiles.
  class HTTPRequest
    attr_reader :method, :path, :decode_content
    attr_accessor :body

    # `path` is a request path, or a URI -- CRuby takes either, and
    # `Net::HTTP::Post.new(uri, "Content-Type" => "application/json")` is the
    # spelling a caller reaches for when it already parsed the URL. A URI
    # contributes its `request_uri` (the path with the query), not its `to_s`,
    # which is the whole URL and would go out as an absolute request line.
    def initialize(method, path, initheader = nil)
      @method = method
      @path = if path.is_a?(URI::Generic)
        path.request_uri
      else
        (path.nil? || path.to_s.empty?) ? "/" : path.to_s
      end
      # Downcased name => value, with the caller's spelling kept beside it for
      # the wire. One key per header name makes a duplicate impossible to
      # construct, which is what "keep the spelling, scan for case variants on
      # every write" was doing by hand. The response half of this file has
      # stored them downcased all along.
      @headers = {}
      @header_names = {}
      @body = ""
      @decode_content = false
      # CRuby's initheader half strips the value before it looks at it, and
      # so raises NoMethodError for anything without `strip`; this package
      # strips a String and lets the other shapes through to `#[]=`, as it
      # already did. So a trailing newline here is whitespace, and the
      # message names the header where `#[]=`'s does not. Both are kept, so a
      # caller rescuing on the message sees the one CRuby raises.
      unless initheader.nil?
        initheader.each do |k, v|
          v = v.strip if v.is_a?(String)
          if v.is_a?(String) && crlf?(v)
            raise ArgumentError, "header #{k} has field value #{v.inspect}, this cannot include CR/LF"
          end
          self[k] = v
        end
      end
      # A caller's Accept-Encoding or Range, even set to nil, leaves the body
      # as it comes. Otherwise the request asks for gzip and deflate and
      # decodes them.
      explicit_encoding = initheader && initheader.keys.any? do |k|
        name = k.to_s.downcase
        name == "accept-encoding" || name == "range"
      end
      unless explicit_encoding
        self["Accept-Encoding"] = "gzip;q=1.0,deflate;q=0.6,identity;q=0.3"
        @decode_content = @method != "HEAD"
      end
      # The two headers CRuby's request carries unless the caller set them.
      self["Accept"] = "*/*" unless key?("accept")
      self["User-Agent"] = "Ruby" unless key?("user-agent")
    end

    # Header names are case-insensitive on the wire, and CRuby's
    # Net::HTTPHeader is case-insensitive on both halves -- as the response
    # half here already is. What is kept, rather than downcased the way the
    # response stores them, is the caller's spelling: for a request that is
    # what goes out on the wire.
    # Header names are case-insensitive on the wire, and CRuby's
    # Net::HTTPHeader is case-insensitive on both halves. Storing under the
    # downcased name makes that a lookup rather than a scan, and makes it
    # impossible to hold one header twice under two spellings -- which is what
    # `Post.new(uri, "content-type" => …)` followed by `req.content_type = …`
    # used to do, sending both and leaving the server to pick.
    def []=(name, value)
      k = name.to_s.downcase
      # An Accept-Encoding set later also leaves the body to the caller.
      @decode_content = false if k == "accept-encoding"
      # nil removes the header, as CRuby's Net::HTTPHeader#[]= does: a Host
      # set to nil lets the default one go out again instead of an empty one.
      if value.nil?
        @headers.delete(k)
        @header_names.delete(k)
        return nil
      end
      # An Array joins with ", ", the way CRuby serves a multi-valued header:
      # `req["Accept"] = %w[a b]` reads back "a, b" and goes out as one line.
      # `to_s` on an Array is its INSPECT form, so without this the wire got
      # `Accept: ["a", "b"]`.
      #
      # CRuby only reaches that path through `#[]=`; its initheader half calls
      # `value.strip` per value and so raises NoMethodError for an Array. This
      # package routes initheader through `#[]=` and therefore accepts one whose
      # elements are clean -- a deliberate divergence, in the permissive
      # direction, and one rule for one value shape rather than two. An element
      # carrying a break is refused below, like any other value.
      text = value.is_a?(Array) ? value.map { |v| v.to_s }.join(", ") : value.to_s
      # The joined text carries every element's bytes, so one check covers a
      # break in any element as well as in a scalar.
      raise ArgumentError, "header field value cannot include CR/LF" if crlf?(text)
      @headers[k] = text
      @header_names[k] = name.to_s
      value
    end

    # A copy, as CRuby's `@header[key].join(", ")` is a new String: appending
    # to what `[]` answers leaves the stored value alone (and the stored one
    # may be a frozen literal).
    def [](name)
      v = @headers[name.to_s.downcase]
      v ? v.dup : nil
    end

    def key?(name)
      @headers.key?(name.to_s.downcase)
    end

    # Whether this kind of request carries a body, CRuby's REQUEST_HAS_BODY.
    # Decided by the method rather than per class, since `Net::HTTP#post` and
    # `Net::HTTP.post_form` build a plain HTTPRequest with the method name.
    def request_body_permitted?
      @method == "POST" || @method == "PUT" || @method == "PATCH"
    end

    # Yields the spelling the caller wrote, not the downcased key: for a
    # request that is what goes out on the wire.
    def each_header
      @headers.each do |k, v|
        name = @header_names[k]
        yield (name.nil? ? k : name), v
      end
      nil
    end

    def content_type=(v)
      self["Content-Type"] = v
    end

    def set_form_data(hash)
      @body = URI.encode_www_form(hash)
      self["Content-Type"] = "application/x-www-form-urlencoded"
      @body
    end

    private

    # A carriage return or a line feed in a header value would end the
    # header on the wire and make whatever follows it a header of its own.
    def crlf?(text)
      text.include?("\r") || text.include?("\n")
    end
  end

  class HTTP
    # CRuby builds requests as Net::HTTP::Get.new(path) and friends. Same
    # spelling here, so the same code compiles.
    class Get < HTTPRequest
      def initialize(path, initheader = nil)
        super("GET", path, initheader)
      end
    end

    class Post < HTTPRequest
      def initialize(path, initheader = nil)
        super("POST", path, initheader)
      end
    end

    class Put < HTTPRequest
      def initialize(path, initheader = nil)
        super("PUT", path, initheader)
      end
    end

    class Patch < HTTPRequest
      def initialize(path, initheader = nil)
        super("PATCH", path, initheader)
      end
    end

    class Delete < HTTPRequest
      def initialize(path, initheader = nil)
        super("DELETE", path, initheader)
      end
    end

    class Head < HTTPRequest
      def initialize(path, initheader = nil)
        super("HEAD", path, initheader)
      end
    end

    attr_reader :address, :port
    attr_accessor :use_ssl, :open_timeout, :read_timeout

    # CRuby's, handed to the TLS context the connection is made with. nil,
    # the default, leaves the context's own, VERIFY_PEER.
    attr_accessor :verify_mode

    # CRuby's `ssl_timeout` is the TLS SESSION timeout (it becomes
    # `SSLContext#timeout`), how long a cached session may be resumed -- not
    # a deadline on the handshake. This client caches no sessions, so there
    # is nothing for it to govern; it is held so a program that sets it
    # compiles and reads back what it wrote. The handshake is bounded by the
    # connect path's own timeouts, as before.
    attr_accessor :ssl_timeout

    # How long, in seconds, a session's idle connection may be reused before
    # `begin_transport` opens a new one, as CRuby's: 2 by default.
    attr_accessor :keep_alive_timeout

    # How many times an idempotent request (GET, HEAD, PUT, DELETE, OPTIONS,
    # TRACE) is sent again when its connection fails before the response
    # arrives, as CRuby's: 1 by default. A connection the server closed while
    # it sat idle fails that way.
    attr_reader :max_retries

    def max_retries=(retries)
      retries = retries.to_int
      raise ArgumentError, "max_retries should be non-negative integer number" if retries < 0
      @max_retries = retries
    end

    # The address to CONNECT to, when it differs from the address the request
    # is addressed to. An application that resolves a hostname itself and then
    # pins the result -- which is how a Rails app defends against DNS
    # rebinding, resolving once and connecting to that literal -- passes the
    # resolved IP here and keeps the hostname for `Host:` and for the TLS
    # certificate. Held as a String rather than nil-or-String: empty means
    # unset. CRuby's reader answers nil there, and this one answers "".
    attr_accessor :ipaddr

    # CRuby's positional proxy arguments, `Net::HTTP.new(host, port, p_addr,
    # p_port, p_user, p_pass)`. This client has no proxy support, so the
    # one value it can honour is the one that asks for none: `nil`, which a
    # program passes to make a connection direct -- the spelling a Rails app
    # uses to keep an egress proxy from re-resolving an address it pinned
    # (see `ipaddr` above). A named proxy raises rather than connecting
    # straight past it. CRuby's default, `:ENV`, reads the proxy from the
    # environment; the default here is nil, which is what this client has
    # always done with the environment.
    def initialize(address, port = 80, p_addr = nil, p_port = nil, p_user = nil, p_pass = nil)
      unless p_addr.nil?
        raise NotImplementedError, "net/http: proxies are not supported (Net::HTTP.new given proxy #{p_addr})"
      end
      @address = address
      @port = port
      @ipaddr = ""
      @use_ssl = false
      @open_timeout = 60
      @read_timeout = 60
      @ssl_timeout = nil
      @keep_alive_timeout = 2
      @max_retries = 1
      @one_shot = false
      @reusable = false
      @last_communicated = 0.0
      @verify_mode = nil
      @debug_output = nil
      @socket = nil
      @tls = nil
      @fresh = false
      @started = false
    end

    # Where the session is traced: the connection opened and closed, each
    # request as written -- its headers and body whole, credentials included
    # -- and each response line as read, with the response body reported by
    # its length. CRuby's `<-` and `->` spelling. Like CRuby's, for debugging
    # only, never in production: the trace carries whatever the request does.
    def set_debug_output(output)
      # Without CRuby's `uplevel: 1`: Spinel has no caller location to prefix.
      warn "Net::HTTP#set_debug_output called after HTTP started" if started?
      @debug_output = output
    end

    def use_ssl?
      @use_ssl
    end

    def started?
      @started
    end

    # Net::HTTP.start(host, port, use_ssl: true) { |http| ... }
    # `ipaddr:` is the connect target; `Host:` and the TLS hostname stay
    # `address`. `nil` reads as unset, which is what CRuby's default is.
    def self.start(address, port = 80, use_ssl: false, ipaddr: nil,
                   open_timeout: 60, read_timeout: 60)
      http = HTTP.new(address, port)
      http.use_ssl = use_ssl
      http.ipaddr = ipaddr.to_s
      http.open_timeout = open_timeout
      http.read_timeout = read_timeout
      http.start { |h| yield h }
    end

    # Net::HTTP.get(uri) -> the body String.
    def self.get(uri)
      get_response(uri).body
    end

    # Net::HTTP.get_response(uri) -> HTTPResponse.
    def self.get_response(uri)
      u = URI(uri)
      https = u.scheme == "https"
      http = HTTP.new(u.host, u.port)
      http.use_ssl = https
      http.start { |h| h.request(HTTPRequest.new("GET", u.request_uri)) }
    end

    def self.post_form(uri, params)
      u = URI(uri)
      req = HTTPRequest.new("POST", u.request_uri)
      req.set_form_data(params)
      http = HTTP.new(u.host, u.port)
      http.use_ssl = (u.scheme == "https")
      http.start { |h| h.request(req) }
    end

    # With a block, the session lasts for the block: CRuby opens it, yields
    # self, closes it in an ensure and answers the block's value. Without
    # one, the session stays open until `finish`. Both open it through
    # do_start, as CRuby's does, so a subclass or a reopening that redefines
    # start around do_start and do_finish keeps working.
    def start
      raise IOError, "HTTP session already opened" if @started
      # CRuby's `return yield(self)` from inside the ensure, spelled as one
      # if/else: Spinel refuses a return through ensure whose value is not
      # the type the method's other exit answers.
      if block_given?
        begin
          do_start
          yield self
        ensure
          do_finish
        end
      else
        do_start
        self
      end
    end

    def do_start
      connect
      @started = true
    end
    private :do_start

    # CRuby's names for the connection lifecycle, and private as they are
    # there: `connect` opens the socket (and the TLS session), and
    # `begin_transport(req)` runs before each request is written, connecting
    # again when the last one was closed. A subclass -- or a module included
    # into one -- may override either and call super to follow a connection
    # through its stages; a pool does, to tell a dead idle connection (a
    # failure before the write) from a request the server may have.
    def connect
      debug "opening connection to #{conn_address}:#{@port}..."
      @socket = connect_with_timeout
      debug "opened"
      if @use_ssl
        begin
          context = OpenSSL::SSL::SSLContext.new
          context.verify_mode = @verify_mode unless @verify_mode.nil?
          tls = OpenSSL::SSL::SSLSocket.new(@socket, context)
        rescue NameError
          raise "net/http: an https request needs the openssl package (require \"openssl\")"
        end
        tls.hostname = @address
        tls.connect
        @tls = tls
      end
      @fresh = true
      nil
    end

    # Closes the session, and raises IOError when none is open, as CRuby's
    # does. The package's own teardown goes through do_finish instead, which
    # is silent: a start whose connect failed has nothing open, and a block
    # given to Net::HTTP.start may have finished the session itself.
    def finish
      raise IOError, "HTTP session not yet started" unless started?
      do_finish
    end

    def do_finish
      close_socket
      @started = false
      nil
    end

    def close_socket
      @tls.sysclose unless @tls.nil?
      @socket.close unless @socket.nil?
      @tls = nil
      @socket = nil
      @fresh = false
      @reusable = false
      nil
    end
    private :do_finish

    # open_timeout, honoured rather than stored: a non-blocking connect and a
    # bounded wait for writability. A timeout of 0 or less means "no limit",
    # which is how CRuby reads nil there.
    def connect_with_timeout
      limit = @open_timeout.nil? ? 0 : @open_timeout
      target = conn_address
      return TCPSocket.new(target, @port) if limit <= 0
      s = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
      begin
        s.connect_nonblock(target, @port)
      rescue IO::WaitWritable
        if IO.select(nil, [s], nil, limit).nil?
          s.close
          raise OpenTimeout
        end
        begin
          s.connect_nonblock(target, @port)
        rescue Errno::EISCONN
          # already connected: the wait above is what completed it
        end
      end
      s
    end

    # CRuby's private hook for the address the socket connects to: the pinned
    # `ipaddr` when one is set, otherwise `address`. A reopening of Net::HTTP
    # that redefines it to connect somewhere else reaches every connect here.
    def conn_address
      @ipaddr.empty? ? address : @ipaddr
    end
    private :conn_address

    def debug(message)
      out = @debug_output
      return if out.nil?
      out << message
      out << "\n"
      nil
    end
    private :debug

    # Wait for the peer to START answering, or give up. Without this a server
    # that accepts the connection and never replies held the caller forever,
    # whatever read_timeout was set to (#4133).
    #
    # Once. Not before every read, and the reason is worth stating: after the
    # first byte arrives the rest of the response is normally already in a
    # buffer this cannot see -- stdio's on the plain path, the TLS record
    # layer's on the other -- so waiting again would time out on data the
    # caller is holding. What this covers is the failure that hangs: a peer
    # that accepts and then says nothing. A stall PART WAY through a response
    # is not covered, and CRuby's per-read timeout does cover it.
    def wait_for_response
      limit = @read_timeout.nil? ? 0 : @read_timeout
      return if limit <= 0
      raise ReadTimeout if IO.select([@socket], nil, nil, limit).nil?
    end

    def get(path, headers = nil)
      req = HTTPRequest.new("GET", path, headers)
      request(req)
    end

    def head(path, headers = nil)
      req = HTTPRequest.new("HEAD", path, headers)
      request(req)
    end

    def post(path, body, headers = nil)
      req = HTTPRequest.new("POST", path, headers)
      req.body = body
      request(req)
    end

    def put(path, body, headers = nil)
      req = Put.new(path, headers)
      req.body = body
      request(req)
    end

    def patch(path, body, headers = nil)
      req = Patch.new(path, headers)
      req.body = body
      request(req)
    end

    # Sends Depth: Infinity unless headers are given, as in CRuby.
    def delete(path, headers = { "Depth" => "Infinity" })
      request(Delete.new(path, headers))
    end

    # Keep the connection alive through the block, then consume an unread body.
    # An explicit block parameter also keeps the no-block spelling callable.
    # A request with no session is the session's only one, and asks the
    # server to close after it, as CRuby's does (write_request).
    def request(req, &blk)
      started = @started
      start unless started
      @one_shot = !started
      res = perform(req)
      blk.call(res) unless blk.nil?
      res.body
      end_transport(req, res)
      res
    ensure
      do_finish unless started
    end

    IDEMPOTENT_METHODS = %w[GET HEAD PUT DELETE OPTIONS TRACE].freeze

    # CRuby's transport_request: a connection that fails before the response
    # arrives (the server closed it while it sat idle, or reset it) is opened
    # again and an idempotent request sent again, max_retries times. A POST
    # is not: the server may have acted on it.
    def perform(req)
      count = 0
      begin
        begin_transport(req)
        write_request(req)
        read_response(req.method, req.decode_content)
      rescue EOFError, IOError, Errno::ECONNRESET, Errno::ECONNABORTED, Errno::EPIPE, ReadTimeout => e
        raise if count >= @max_retries || !IDEMPOTENT_METHODS.include?(req.method)
        debug "Conn close because of error #{e.message}, and retry"
        close_socket
        count += 1
        retry
      end
    end

    # A connection is reused while it is alive: kept after the last response
    # (end_transport), idle for no longer than keep_alive_timeout, and with
    # nothing to read, which an idle connection has only when the server
    # closed it or said something unasked. Otherwise a new one is opened.
    def begin_transport(req)
      if @socket.nil?
        connect
      elsif !@fresh
        reuse = @reusable
        if reuse && Process.clock_gettime(Process::CLOCK_MONOTONIC) - @last_communicated > @keep_alive_timeout
          debug "Conn close because of keep_alive_timeout"
          reuse = false
        end
        if reuse && !IO.select([@socket], nil, nil, 0).nil?
          debug "Conn close because of EOF"
          reuse = false
        end
        unless reuse
          close_socket
          connect
        end
      end
      @fresh = false
      @reusable = false
      nil
    end

    # After the response's body is read: the connection is kept for the next
    # request unless either side asked to close it, or the server speaks
    # HTTP/1.0 and did not ask to keep it (CRuby's keep_alive?).
    def end_transport(req, res)
      keep = !@one_shot && !req["connection"].to_s.downcase.include?("close") && !@socket.nil?
      if keep
        conn = res["connection"].to_s.downcase
        keep = res.http_version == "1.0" || res.http_version.empty? ? conn.include?("keep-alive") : !conn.include?("close")
      end
      @reusable = keep
      if keep
        debug "Conn keep-alive"
        @last_communicated = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      else
        debug "Conn close"
        close_socket
      end
      nil
    end
    private :connect, :begin_transport, :end_transport, :close_socket

    # ---- the wire ----

    def wire_write(s)
      if @tls.nil?
        @socket.write(s)
      else
        @tls.write(s)
      end
      nil
    end

    def wire_gets
      @tls.nil? ? @socket.gets : @tls.gets
    end

    def wire_read(n)
      @tls.nil? ? @socket.readpartial(n) : @tls.readpartial(n)
    rescue EOFError
      nil
    end

    def wire_read_all
      @tls.nil? ? @socket.read : @tls.read
    end

    def write_request(req)
      out = String.new
      out << "#{req.method} #{req.path} HTTP/1.1\r\n"
      # The default Host goes out only when the request carries none, as
      # CRuby's `req['host'] ||= addr_port`. The port belongs in it unless it
      # is the scheme's default, which is what a virtual host on a
      # non-standard port depends on.
      unless req.key?("host")
        default = @use_ssl ? 443 : 80
        out << (@port == default ? "Host: #{@address}\r\n" : "Host: #{@address}:#{@port}\r\n")
      end
      have_len = false
      req.each_header do |k, v|
        have_len = true if k.downcase == "content-length"
        out << "#{k}: #{v}\r\n"
      end
      body = req.body.to_s
      # A request that carries a body states its length even when it is
      # zero, as CRuby's does; some servers answer 411 without it.
      out << "Content-Length: #{body.bytesize}\r\n" if !have_len && (req.request_body_permitted? || !body.empty?)
      out << "Connection: close\r\n" if @one_shot && !req.key?("connection")
      out << "\r\n"
      out << body
      debug "<- #{out.dump}"
      wire_write(out)
    end

    def read_response(method, decode_content)
      wait_for_response
      version = ""
      code = ""
      message = ""
      headers = {}
      fields = {}
      # An informational response (100 Continue, 103 Early Hints) comes
      # before the final one on the same connection and is not the answer:
      # read past it, as CRuby does, or its successor was taken for its body.
      # 101 Switching Protocols is final -- the connection is someone else's
      # after it.
      loop do
        status = wire_gets
        raise EOFError, "end of file reached" if status.nil?
        debug "-> #{status.dump}"
        # CRuby's check of the status line: HTTP, an optional version, a
        # three-digit code and an optional reason.
        line = status.chomp
        m = /\AHTTP(?:\/(\d+\.\d+))?\s+(\d\d\d)(?:\s+(.*))?\z/.match(line)
        raise HTTPBadResponse, "wrong status line: #{line.dump}" if m.nil?
        version = m[1].to_s
        code = m[2].to_s
        message = m[3].to_s.strip

        headers = {}
        fields = {}
        while (line = wire_gets)
          debug "-> #{line.dump}"
          line = line.strip
          break if line.empty?
          ci = line.index(":")
          next if ci.nil?
          name = line[0, ci].downcase
          value = line[(ci + 1)..-1].to_s.strip
          (fields[name] ||= []) << value
          headers[name] = fields[name].join(", ")
        end
        break unless code[0, 1] == "1" && code != "101"
      end

      # A HEAD answer, a 1xx, 204 and 304 carry no body whatever their
      # headers say (RFC 9112 s6.3): reading one waited for bytes that never
      # come, until the server closed the connection. Their #body is nil,
      # as CRuby's is.
      bodyless = method == "HEAD" || code[0, 1] == "1" || code == "204" || code == "304"
      res = build_response(version, code, message, headers, nil, fields)
      res.reading_body(self, decode_content) unless bodyless
      res
    end

    def read_entity_body(headers, fields, dest, decode_content)
      length_before = headers["content-length"]
      encoded = decode_content && !headers.key?("content-range") &&
        ["gzip", "x-gzip", "deflate"].include?(headers["content-encoding"].to_s.downcase)
      if encoded
        body = String.new
        read_framed_body(headers, body)
        dest << decode_body(headers, body)
      else
        read_framed_body(headers, dest)
        if decode_content && !headers.key?("content-range") &&
            ["identity", "none"].include?(headers["content-encoding"].to_s.downcase)
          headers.delete("content-encoding")
        end
      end
      if decode_content
        # decode_body may drop Content-Encoding and restate Content-Length;
        # the separate values follow only what it changed.
        fields.delete("content-encoding") unless headers.key?("content-encoding")
        if headers.key?("content-length") && headers["content-length"] != length_before
          fields["content-length"] = [headers["content-length"]]
        end
      end
      dest
    end

    def read_framed_body(headers, dest)
      if headers["transfer-encoding"].to_s.downcase.split(",").last.to_s.strip == "chunked"
        read_chunked(dest)
      elsif headers.key?("content-length")
        read_length(headers["content-length"].to_i, dest)
      else
        while (chunk = wire_read(16384))
          break if chunk.empty?
          dest << chunk
        end
      end
    end

    def read_length(n, dest)
      while n > 0
        chunk = wire_read([n, 16384].min).to_s
        break if chunk.empty?
        dest << chunk
        n -= chunk.bytesize
      end
    end

    # Inflates a gzip or deflate body once it is read whole. A Content-Range
    # answer is a slice of the encoded body, so it stays as it came.
    def decode_body(headers, body)
      return body if headers.key?("content-range")
      encoding = headers["content-encoding"].to_s.downcase
      case encoding
      when "gzip", "x-gzip", "deflate"
        unless body.empty?
          inflater = Zlib::Inflate.new(32 + Zlib::MAX_WBITS)
          begin
            body = inflater.inflate(body)
          ensure
            inflater.close
          end
        end
        headers.delete("content-encoding")
        headers["content-length"] = body.bytesize.to_s if headers.key?("content-length")
      when "identity", "none"
        headers.delete("content-encoding")
      end
      body
    end

    # The class a status code names. Specific first, then the family by its
    # leading digit, so an unlisted 2xx is still a Net::HTTPSuccess and
    # `res.is_a?(Net::HTTPSuccess)` answers what it should.
    def build_response(version, code, message, headers, body, fields = nil)
      case code
      when "200" then return HTTPOK.new(version, code, message, headers, body, fields)
      when "201" then return HTTPCreated.new(version, code, message, headers, body, fields)
      when "202" then return HTTPAccepted.new(version, code, message, headers, body, fields)
      when "204" then return HTTPNoContent.new(version, code, message, headers, body, fields)
      when "301" then return HTTPMovedPermanently.new(version, code, message, headers, body, fields)
      when "302" then return HTTPFound.new(version, code, message, headers, body, fields)
      when "304" then return HTTPNotModified.new(version, code, message, headers, body, fields)
      when "307" then return HTTPTemporaryRedirect.new(version, code, message, headers, body, fields)
      when "308" then return HTTPPermanentRedirect.new(version, code, message, headers, body, fields)
      when "400" then return HTTPBadRequest.new(version, code, message, headers, body, fields)
      when "401" then return HTTPUnauthorized.new(version, code, message, headers, body, fields)
      when "403" then return HTTPForbidden.new(version, code, message, headers, body, fields)
      when "404" then return HTTPNotFound.new(version, code, message, headers, body, fields)
      when "405" then return HTTPMethodNotAllowed.new(version, code, message, headers, body, fields)
      when "409" then return HTTPConflict.new(version, code, message, headers, body, fields)
      when "410" then return HTTPGone.new(version, code, message, headers, body, fields)
      when "422" then return HTTPUnprocessableEntity.new(version, code, message, headers, body, fields)
      when "429" then return HTTPTooManyRequests.new(version, code, message, headers, body, fields)
      when "500" then return HTTPInternalServerError.new(version, code, message, headers, body, fields)
      when "502" then return HTTPBadGateway.new(version, code, message, headers, body, fields)
      when "503" then return HTTPServiceUnavailable.new(version, code, message, headers, body, fields)
      when "504" then return HTTPGatewayTimeout.new(version, code, message, headers, body, fields)
      end
      case code[0, 1]
      when "1" then HTTPInformation.new(version, code, message, headers, body, fields)
      when "2" then HTTPSuccess.new(version, code, message, headers, body, fields)
      when "3" then HTTPRedirection.new(version, code, message, headers, body, fields)
      when "4" then HTTPClientError.new(version, code, message, headers, body, fields)
      when "5" then HTTPServerError.new(version, code, message, headers, body, fields)
      else HTTPResponse.new(version, code, message, headers, body, fields)
      end
    end

    # Chunked transfer: each chunk is a hex length line, the bytes, then CRLF.
    # A zero length ends it, and the trailer that may follow is read to the
    # blank line so the connection is left where the caller expects.
    def read_chunked(out)
      loop do
        line = wire_gets
        break if line.nil?
        size = line.strip.split(";")[0].to_s.to_i(16)
        if size == 0
          while (t = wire_gets)
            break if t.strip.empty?
          end
          break
        end
        read_length(size, out)
        wire_gets                    # the CRLF after the chunk
      end
      out
    end
  end
end

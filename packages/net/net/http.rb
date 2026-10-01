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
# * HTTP/1.1 with `Connection: close`, one request per connection. There is no
#   keep-alive, no pipelining and no HTTP/2
# * no proxy support, no cookie jar, no automatic redirect following
#   (#get_response hands back the 3xx and its Location, as CRuby's does)
# * no streaming body block on #request; the body is read whole
# * chunked transfer decoding is here; content-encoding (gzip) is not
require "socket"
require "uri"

module Timeout
  # CRuby's base for both of the timeouts below, so `rescue Timeout::Error`
  # catches either -- which is what a caller with a deadline writes.
  class Error < RuntimeError
  end
end

module Net
  class HTTPError < StandardError
  end

  # Raised when the peer accepts the connection and then says nothing for
  # read_timeout seconds, and when a connection cannot be established within
  # open_timeout. Same classes and same base as CRuby's.
  class ReadTimeout < Timeout::Error
    def message = "Net::ReadTimeout"
  end

  class OpenTimeout < Timeout::Error
    def message = "Net::OpenTimeout"
  end

  # The response. `code` is a String, as CRuby has it ("200", not 200), and
  # header lookup through #[] is case-insensitive.
  class HTTPResponse
    attr_reader :http_version, :code, :message, :body

    def initialize(http_version, code, message, headers, body)
      @http_version = http_version
      @code = code
      @message = message
      @headers = headers          # downcased name => value
      @body = body
    end

    def [](name)
      @headers[name.to_s.downcase]
    end

    def key?(name)
      @headers.key?(name.to_s.downcase)
    end

    def each_header
      @headers.each { |k, v| yield k, v }
      nil
    end

    def content_type
      @headers["content-type"]
    end
  end

  # The response family. CRuby's success test is `res.is_a?(Net::HTTPSuccess)`
  # rather than a predicate on the code, so the classes have to exist for that
  # line to compile. The families are all here; of the per-code classes only
  # the ones a client actually names are, which is the usual subset rule.
  class HTTPInformation < HTTPResponse; end
  class HTTPSuccess < HTTPResponse; end
  class HTTPRedirection < HTTPResponse; end
  class HTTPClientError < HTTPResponse; end
  class HTTPServerError < HTTPResponse; end

  class HTTPOK < HTTPSuccess; end
  class HTTPCreated < HTTPSuccess; end
  class HTTPNoContent < HTTPSuccess; end
  class HTTPMovedPermanently < HTTPRedirection; end
  class HTTPFound < HTTPRedirection; end
  class HTTPBadRequest < HTTPClientError; end
  class HTTPUnauthorized < HTTPClientError; end
  class HTTPForbidden < HTTPClientError; end
  class HTTPNotFound < HTTPClientError; end
  class HTTPInternalServerError < HTTPServerError; end

  # A request. CRuby builds these as Net::HTTP::Get.new(path) and friends;
  # the same shape is here so the same code compiles.
  class HTTPRequest
    attr_reader :method, :path
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

    # CRuby's `ssl_timeout` is the TLS SESSION timeout (it becomes
    # `SSLContext#timeout`), how long a cached session may be resumed -- not
    # a deadline on the handshake. This client caches no sessions, so there
    # is nothing for it to govern; it is held so a program that sets it
    # compiles and reads back what it wrote. The handshake is bounded by the
    # connect path's own timeouts, as before.
    attr_accessor :ssl_timeout

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
      @socket = nil
      @tls = nil
      @fresh = false
      @started = false
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
      http.start
      begin
        yield http
      ensure
        http.finish
      end
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
      http.start
      begin
        http.request(HTTPRequest.new("GET", u.request_uri))
      ensure
        http.finish
      end
    end

    def self.post_form(uri, params)
      u = URI(uri)
      req = HTTPRequest.new("POST", u.request_uri)
      req.set_form_data(params)
      http = HTTP.new(u.host, u.port)
      http.use_ssl = (u.scheme == "https")
      http.start
      begin
        http.request(req)
      ensure
        http.finish
      end
    end

    def start
      open_connection
      @started = true
      self
    end

    def open_connection
      @socket = connect_with_timeout
      if @use_ssl
        begin
          tls = OpenSSL::SSL::SSLSocket.new(@socket)
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

    def finish
      @tls.sysclose unless @tls.nil?
      @socket.close unless @socket.nil?
      @tls = nil
      @socket = nil
      @fresh = false
      @started = false
      nil
    end

    # open_timeout, honoured rather than stored: a non-blocking connect and a
    # bounded wait for writability. A timeout of 0 or less means "no limit",
    # which is how CRuby reads nil there.
    def connect_with_timeout
      limit = @open_timeout.nil? ? 0 : @open_timeout
      target = @ipaddr.empty? ? @address : @ipaddr
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
      req = HTTPRequest.new("GET", path)
      headers.each { |k, v| req[k] = v } unless headers.nil?
      request(req)
    end

    def head(path, headers = nil)
      req = HTTPRequest.new("HEAD", path)
      headers.each { |k, v| req[k] = v } unless headers.nil?
      request(req)
    end

    def post(path, body, headers = nil)
      req = HTTPRequest.new("POST", path)
      req.body = body
      headers.each { |k, v| req[k] = v } unless headers.nil?
      request(req)
    end

    # A block gets the response, as CRuby's does. CRuby streams the body to it;
    # this reads the body whole first, so the block sees a complete response --
    # the difference is when the bytes arrive, not what the block is handed.
    # The transport lives in `perform` so that the reconnect recursion below
    # has no block to forward and this method has exactly one place to call it.
    # `&blk` rather than `yield ... if block_given?`: the latter makes this a
    # YIELDING method, which is inlined at its call sites, and the no-block
    # spelling then found no arm here (`undefined method 'request'` at run
    # time). A declared block parameter keeps one ordinary function for both
    # spellings.
    def request(req, &blk)
      res = perform(req)
      blk.call(res) unless blk.nil?
      res
    end

    def perform(req)
      # CRuby opens a connection for a #request on an unstarted Net::HTTP, runs
      # the request over it and closes it again -- so `Net::HTTP.new(host,
      # port).request(req)` works without a `start`, and is the spelling a
      # caller writes when the connection is configured somewhere other than
      # where the request is sent. Doing that here rather than raising keeps
      # the two spellings interchangeable, as they are there.
      unless @started
        # `start` is INSIDE the begin: `open_connection` assigns @socket and
        # only then completes the TLS handshake, so a handshake failure raises
        # with a live socket that nothing else will close. `finish` is a no-op
        # when there is nothing open, which is the other way start can fail.
        begin
          start
          return perform(req)
        ensure
          finish
        end
      end
      # Every request goes out with `Connection: close`, so the server hangs
      # up after answering and the socket a second request would use is dead.
      # CRuby reconnects transparently in that situation, and a caller writing
      # `start { |http| http.get("/a"); http.get("/b") }` -- which is the
      # idiom -- has no reason to know. This is one connection per request
      # rather than keep-alive; what it is not is a failure on the second one.
      reconnect unless @fresh
      @fresh = false
      write_request(req)
      read_response(req.method)
    end

    def reconnect
      @tls.sysclose unless @tls.nil?
      @socket.close unless @socket.nil?
      @tls = nil
      @socket = nil
      open_connection
    end

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
      @tls.nil? ? @socket.read(n) : @tls.read(n)
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
      out << "Content-Length: #{body.bytesize}\r\n" if !have_len && !body.empty?
      out << "Connection: close\r\n"
      out << "\r\n"
      out << body
      wire_write(out)
    end

    def read_response(method)
      wait_for_response
      version = ""
      code = ""
      message = ""
      headers = {}
      # An informational response (100 Continue, 103 Early Hints) comes
      # before the final one on the same connection and is not the answer:
      # read past it, as CRuby does, or its successor was taken for its body.
      # 101 Switching Protocols is final -- the connection is someone else's
      # after it.
      loop do
        status = wire_gets
        raise HTTPError, "no response from #{@address}" if status.nil?
        parts = status.strip.split(" ")
        version = parts[0].to_s
        code = parts.length > 1 ? parts[1].to_s : ""
        message = parts.length > 2 ? parts[2..-1].join(" ") : ""

        headers = {}
        while (line = wire_gets)
          line = line.strip
          break if line.empty?
          ci = line.index(":")
          next if ci.nil?
          headers[line[0, ci].downcase] = line[(ci + 1)..-1].to_s.strip
        end
        break unless code[0, 1] == "1" && code != "101"
      end

      # A HEAD answer, a 1xx, 204 and 304 carry no body whatever their
      # headers say (RFC 9112 s6.3): reading one waited for bytes that never
      # come, until the server closed the connection. Their #body is nil,
      # as CRuby's is.
      bodyless = method == "HEAD" || code[0, 1] == "1" || code == "204" || code == "304"
      body =
        if bodyless
          nil
        elsif headers["transfer-encoding"].to_s.downcase == "chunked"
          read_chunked
        elsif headers.key?("content-length")
          n = headers["content-length"].to_i
          n > 0 ? wire_read(n).to_s : ""
        else
          wire_read_all.to_s
        end

      build_response(version, code, message, headers, body)
    end

    # The class a status code names. Specific first, then the family by its
    # leading digit, so an unlisted 2xx is still a Net::HTTPSuccess and
    # `res.is_a?(Net::HTTPSuccess)` answers what it should.
    def build_response(version, code, message, headers, body)
      case code
      when "200" then return HTTPOK.new(version, code, message, headers, body)
      when "201" then return HTTPCreated.new(version, code, message, headers, body)
      when "204" then return HTTPNoContent.new(version, code, message, headers, body)
      when "301" then return HTTPMovedPermanently.new(version, code, message, headers, body)
      when "302" then return HTTPFound.new(version, code, message, headers, body)
      when "400" then return HTTPBadRequest.new(version, code, message, headers, body)
      when "401" then return HTTPUnauthorized.new(version, code, message, headers, body)
      when "403" then return HTTPForbidden.new(version, code, message, headers, body)
      when "404" then return HTTPNotFound.new(version, code, message, headers, body)
      when "500" then return HTTPInternalServerError.new(version, code, message, headers, body)
      end
      case code[0, 1]
      when "1" then HTTPInformation.new(version, code, message, headers, body)
      when "2" then HTTPSuccess.new(version, code, message, headers, body)
      when "3" then HTTPRedirection.new(version, code, message, headers, body)
      when "4" then HTTPClientError.new(version, code, message, headers, body)
      when "5" then HTTPServerError.new(version, code, message, headers, body)
      else HTTPResponse.new(version, code, message, headers, body)
      end
    end

    # Chunked transfer: each chunk is a hex length line, the bytes, then CRLF.
    # A zero length ends it, and the trailer that may follow is read to the
    # blank line so the connection is left where the caller expects.
    def read_chunked
      out = String.new
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
        chunk = wire_read(size)
        break if chunk.nil?
        out << chunk
        wire_gets                    # the CRLF after the chunk
      end
      out
    end
  end
end

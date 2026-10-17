# Spinel bundled `uri`.
#
# CRuby's uri: URI.parse answers the class of the scheme -- URI::HTTP,
# HTTPS, FTP, File, WS, WSS, MailTo, LDAP, LDAPS, one URI.register_scheme
# added, or URI::Generic -- with its component readers and writers, #to_s,
# #merge / #route_to, #normalize, #find_proxy and the rest; URI.split and
# URI.join on CRuby's RFC 3986 grammar, the RFC 2396 parser, the www-form
# and URI-component codecs, URI.extract / URI.regexp and URI.get_encoding.
# What is not here is absent the way a subset is absent things, and a
# program naming it fails to compile rather than at run time:
#
# * the URI.escape family CRuby removed, the obsolete constants CRuby
#   answers through const_missing, and URI.parser= (which rebinds
#   constants)
require "socket"

module URI
  class Error < StandardError
  end

  class InvalidURIError < Error
  end

  class InvalidComponentError < Error
  end

  class BadURIError < Error
  end

  class Generic
    attr_reader :scheme, :userinfo, :host, :port, :path, :query, :fragment
    # What follows the scheme of a URI with no hierarchy ("isbn:1" in
    # "urn:isbn:1"); nil for every other URI.
    attr_reader :opaque

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      @parser = nil
      @scheme = scheme.nil? ? nil : scheme.downcase
      # An empty userinfo ("http://@h/") has no user, as CRuby splits it.
      @userinfo = userinfo.nil? || userinfo.empty? ? nil : userinfo
      @host = host
      @port = port
      @path = path
      @opaque = nil
      # CRuby's initialize sends both through their writers, which escape
      # what a query or a fragment may not carry: "?a b" is "?a%20b".
      @query = URI.normalize_query(query)
      @fragment = URI.normalize_fragment(fragment)
      @port = default_port if @port.nil?
    end

    # The component writers, with CRuby's checks against the grammar of
    # the parser that made the URI (RFC 3986's, unless the RFC 2396 parser
    # parsed it): a bad component raises InvalidComponentError, one the
    # URI's other parts rule out InvalidURIError. Setting the host or the
    # port drops the userinfo, as CRuby's writers do.
    def scheme=(v)
      if !v.nil? && !grammar_match?(:SCHEME, v)
        raise InvalidComponentError, "bad component(expected scheme component): #{v}"
      end
      set_scheme(v)
      v
    end

    def userinfo=(v)
      return nil if v.nil?
      u = nil
      pw = nil
      if v.is_a?(Array)
        u = v[0]
        pw = v[1]
      else
        u, pw = URI.split_userinfo(v)
      end
      check_user(u)
      check_password(pw, u)
      set_userinfo(URI.join_userinfo(u, pw))
      v
    end

    # The user, a ":" in it splitting off the password, as CRuby's #user=
    # sets both from one string.
    def user=(v)
      check_user(v)
      set_userinfo(v)
      v
    end

    # The password after the user; there must be a user.
    def password=(v)
      check_password(v, user)
      set_userinfo(URI.join_userinfo(user, v))
      v
    end

    def host=(v)
      check_host(v)
      set_host(v)
      set_userinfo(nil)
      v
    end

    # The host, an IPv6 address given bare put in brackets.
    def hostname=(v)
      v = "[#{v}]" if !v.nil? && !(v.start_with?("[") && v.end_with?("]")) && v.index(":")
      self.host = v
    end

    def port=(v)
      unless v.nil?
        raise InvalidURIError, "cannot set port with registry or opaque" unless @opaque.nil?
        if !v.is_a?(Integer) && !grammar_match?(:PORT, v)
          raise InvalidComponentError, "bad component(expected port component): #{v.inspect}"
        end
      end
      set_port(v.is_a?(Integer) || v.nil? ? v : (v.empty? ? nil : v.to_i))
      set_userinfo(nil)
      port
    end

    def registry=(v)
      raise InvalidURIError, "cannot set registry"
    end

    def registry
      nil
    end

    # An absolute path once the URI has a scheme (FTP's is relative to the
    # login directory, so either kind), and no path beside an opaque part.
    def path=(v)
      raise InvalidURIError, "path conflicts with opaque" if !v.nil? && !@opaque.nil?
      if !v.nil? && !v.empty? && !grammar_match?(:ABS_PATH, v)
        if !@scheme.nil? && @scheme != "ftp"
          raise InvalidComponentError, "bad component(expected absolute path component): #{v}"
        elsif !grammar_match?(:REL_PATH, v)
          raise InvalidComponentError, "bad component(expected relative path component): #{v}"
        end
      end
      set_path(v)
      v
    end

    def query=(v)
      return set_query(nil) if v.nil?
      raise InvalidURIError, "query conflicts with opaque" unless @opaque.nil?
      set_query(URI.normalize_query(v))
    end

    def opaque=(v)
      unless v.nil?
        if !@host.nil? || !@port.nil? || !@userinfo.nil? || !@path.nil?
          raise InvalidURIError, "cannot set opaque with host, port, userinfo or path"
        elsif !grammar_match?(:OPAQUE, v)
          raise InvalidComponentError, "bad component(expected opaque component): #{v}"
        end
      end
      set_opaque(v)
      v
    end

    def fragment=(v)
      set_fragment(URI.normalize_fragment(v))
    end

    # The parser whose grammar the writers check against.
    def parser
      @parser.nil? ? RFC3986_PARSER : @parser
    end

    def parser=(v)
      @parser = v
    end

    # The unchecked setters, as CRuby's protected set_* are: what #merge,
    # #route_from and the parsers assemble a URI with.
    def set_scheme(v)
      @scheme = v.nil? ? nil : v.downcase
    end

    def set_userinfo(v)
      @userinfo = v.nil? || v.empty? ? nil : v
    end

    def set_host(v)
      @host = v
    end

    def set_port(v)
      @port = v
    end

    def set_path(v)
      @path = v
    end

    def set_query(v)
      @query = v
    end

    def set_opaque(v)
      @opaque = v
    end

    def set_fragment(v)
      @fragment = v
    end

    # true when the URI names a scheme -- a full URL rather than a bare
    # path such as a request line's "/hello"
    def absolute?
      !@scheme.nil? && !@scheme.empty?
    end

    # The port a scheme's class implies (nil for a scheme with none); a
    # parsed URI with no port of its own takes it.
    def self.default_port
      nil
    end

    def default_port
      self.class.default_port
    end

    # `URI::Generic.build(scheme: "http", host: "x.com")`: a URI from its
    # components, of this class whatever the scheme -- a Hash (or keywords)
    # by name, or an Array of all nine in #component's order.
    def self.build(args = nil, **kw)
      h = args.nil? ? kw : args
      # zip, not stores into a fresh {}: building it that way breaks the
      # typing of an app's String-keyed hashes (Campfire's request headers).
      if h.is_a?(Array) && h.size == 9
        h = [:scheme, :userinfo, :host, :port, :registry, :path, :opaque, :query, :fragment].zip(h).to_h
      elsif !h.is_a?(Hash)
        raise ArgumentError, "expected Array of or Hash of components of #{self} (#{component.join(', ')})"
      end
      from_components(h)
    end

    # A URI of this class from a components Hash, each component through
    # its checked writer in the order CRuby's build sends them (an IPv6
    # host bracketed, a port String read as a number), a port not given
    # being the scheme's default.
    def self.from_components(h)
      g = Generic.new(nil, nil, nil, nil, nil, nil, nil)
      g.scheme = h[:scheme]
      g.hostname = h[:host]
      g.port = h[:port]
      check_build_userinfo(h[:userinfo])
      g.userinfo = h[:userinfo]
      g.path = h[:path]
      g.query = h[:query]
      g.opaque = h[:opaque]
      g.fragment = h[:fragment]
      path = g.path
      path = "" if path.nil? && g.opaque.nil?
      u = new(g.scheme, g.userinfo, g.host, g.port, path, g.query, g.fragment)
      u.check_host(u.host)
      u.set_opaque(g.opaque) unless g.opaque.nil?
      u
    end

    # File refuses a userinfo; every other class takes one.
    def self.check_build_userinfo(v)
      nil
    end

    # CRuby's Util.make_components_hash for a scheme class's build: an
    # Array of its components after the scheme, in order, or a Hash; the
    # scheme is the class's name.
    def self.components_hash(args, kw)
      h = args.nil? ? kw : args
      comps = component
      if h.is_a?(Array) && h.size == comps.size - 1
        h = comps[1..-1].zip(h).to_h
      elsif !h.is_a?(Hash)
        raise ArgumentError, "expected Array of or Hash of components of #{self} (#{comps[1..-1].join(', ')})"
      end
      h.merge({ scheme: name.sub(/\A.*::/, "").downcase })
    end

    # The parts of the userinfo "user:password" either side of its first ":".
    def user
      return nil if @userinfo.nil?
      i = @userinfo.index(":")
      i ? @userinfo[0, i] : @userinfo
    end

    def password
      return nil if @userinfo.nil?
      i = @userinfo.index(":")
      i ? @userinfo[(i + 1)..-1] : nil
    end

    def relative?
      !absolute?
    end

    # The scheme and the host in lower case, and "/" for an empty path, in
    # place; #normalize answers a normalized copy.
    def normalize!
      @path = "/" if !@path.nil? && @path.empty?
      @scheme = @scheme.downcase unless @scheme.nil?
      # CRuby's answer is its last statement: the host it lowered, else nil.
      return nil if @host.nil? || @host == @host.downcase
      @host = @host.downcase
    end

    def normalize
      u = dup
      u.normalize!
      u
    end

    # the host without the brackets an IPv6 address carries in a URI, as
    # CRuby's URI::Generic#hostname answers it
    def hostname
      v = @host
      v && v.start_with?("[") && v.end_with?("]") ? v[1..-2] : v
    end

    # CRuby's #to_s: the authority's "//" whenever there is a host (and
    # for file and postgres URIs, as "file:///etc"), a default port left
    # off, and a "/" between an authority and a relative path.
    def to_s
      s = String.new
      s << "#{@scheme}:" unless @scheme.nil?
      if @opaque.nil?
        s << "//" if !@host.nil? || @scheme == "file" || @scheme == "postgres"
        s << "#{userinfo}@" unless userinfo.nil?
        s << @host unless @host.nil?
        s << ":#{@port}" if !@port.nil? && @port != default_port
        p = @path.to_s
        s << "/" if (!@host.nil? || !@port.nil?) && !p.empty? && !p.start_with?("/")
        s << p
        s << "?#{@query}" unless @query.nil?
      else
        s << @opaque
      end
      s << "##{@fragment}" unless @fragment.nil?
      s
    end

    # Resolves +oth+ (a URI or a String) against this URI, as CRuby's
    # URI::Generic#merge does: an absolute reference is returned as it is,
    # one with an authority ("//host/x") keeps only the scheme, an absolute
    # path replaces the path, and a relative path is merged with this one's
    # directory, "." and ".." segments removed. The query and fragment come
    # from the reference. Raises BadURIError when both URIs are relative.
    def merge(oth)
      rel = URI.for_merge(oth)
      return rel if rel.absolute?
      raise BadURIError, "both URI are relative" unless absolute?

      base = dup
      authority = !rel.userinfo.nil? || !rel.host.nil? || !rel.port.nil?

      # Only a fragment (or nothing at all): this URI with that fragment.
      if (rel.path.nil? || rel.path.empty?) && !authority && rel.query.nil?
        base.set_fragment(rel.fragment) unless rel.fragment.nil?
        return base
      end

      base.set_query(nil)
      base.set_fragment(nil)
      if authority
        # The reference's authority replaces the base's whole: a port it
        # leaves out is the scheme's default, not the base's.
        base.set_userinfo(rel.userinfo)
        base.set_host(rel.host)
        base.set_port(rel.port.nil? ? base.default_port : rel.port)
        base.set_path(rel.path.to_s)
      elsif base.opaque.nil?
        base.set_path(merge_path(base.path.to_s, rel.path.to_s))
      end
      base.set_query(rel.query) unless rel.query.nil?
      base.set_fragment(rel.fragment) unless rel.fragment.nil?
      base
    end
    def +(oth)
      merge(oth)
    end

    private

    # The path of #merge for a relative-path reference: +rel+ appended to
    # the directory of +base+, then its "." and ".." segments resolved; a
    # ".." that would climb above the root is dropped.
    def merge_path(base, rel)
      base_path = base.split("/", -1)
      rel_path = rel.split("/", -1)

      base_path << "" if base_path.last == ".."
      while (i = base_path.index(".."))
        base_path.slice!(i - 1, 2)
      end

      first = rel_path.first
      if first && first.empty?
        base_path.clear
        rel_path.shift
      end

      rel_path.push("") if rel_path.last == "." || rel_path.last == ".."
      rel_path.delete(".")

      tmp = []
      rel_path.each do |x|
        if x == ".." && !(tmp.empty? || tmp.last == "..")
          tmp.pop
        else
          tmp << x
        end
      end

      add_trailer_slash = !tmp.empty?
      if base_path.empty?
        base_path = [""]
      elsif add_trailer_slash
        base_path.pop
      end
      while (x = tmp.shift)
        if x == ".."
          base_path.pop if base_path.size > 1
        else
          base_path << x
          tmp.each { |t| base_path << t }
          add_trailer_slash = false
          break
        end
      end
      base_path.push("") if add_trailer_slash
      base_path.join("/")
    end

    public

    # #merge, in place: nil when it changes nothing.
    def merge!(oth)
      t = merge(oth)
      return nil if self == t
      replace!(t)
      self
    end

    # A relative reference from +oth+ to this URI -- the inverse of #merge,
    # `URI("http://a/b/c/d") - "http://a/b/"` is "c/d". A URI of another
    # scheme, or on another host, comes back absolute.
    def route_from(oth)
      oth = URI.for_merge(oth)
      raise BadURIError, "relative URI: #{self}" if relative?
      raise BadURIError, "relative URI: #{oth}" if oth.relative?
      return dup if @scheme != oth.scheme

      rel = Generic.new(nil, @userinfo, @host, @port, @path, @query, @fragment)
      rel.set_opaque(@opaque)
      if rel.userinfo != oth.userinfo || rel.host.to_s.downcase != oth.host.to_s.downcase || rel.port != oth.port
        return dup if @userinfo.nil? && @host.nil?
        rel.set_port(nil) if rel.port == oth.default_port
        return rel
      end
      rel.set_userinfo(nil)
      rel.set_host(nil)
      rel.set_port(nil)
      if !rel.path.nil? && rel.path == oth.path
        rel.set_path("")
        rel.set_query(nil) if rel.query == oth.query
        return rel
      elsif !rel.opaque.nil? && rel.opaque == oth.opaque
        rel.set_opaque("")
        rel.set_query(nil) if rel.query == oth.query
        return rel
      end
      rel.set_path(route_from_path(oth.path.to_s, @path.to_s))
      rel.set_path("") if rel.path == "./" && !@query.nil?
      rel
    end

    def -(oth)
      route_from(oth)
    end

    # The reference from this URI to +oth+: `URI("http://a/b/").route_to("http://a/b/c")` is "c".
    def route_to(oth)
      URI.for_merge(oth).route_from(self)
    end

    # The components of this URI's class, by name: `select(:host, :port)`.
    def select(*components)
      components.map do |c|
        unless component.include?(c)
          raise ArgumentError, "expected of components of #{self.class} (#{self.class.component.join(', ')})"
        end
        component_value(c)
      end
    end

    def self.component
      [:scheme, :userinfo, :host, :port, :registry, :path, :opaque, :query, :fragment]
    end

    def component
      self.class.component
    end

    # Generic.build from a Hash or the components in order, as CRuby's build2.
    def self.build2(args)
      build(args)
    end

    def self.use_registry
      false
    end

    # A String operand of a URI operator is parsed: `"http://a/" + uri`.
    def coerce(oth)
      raise TypeError, "#{oth.class} can't be coerced into #{self.class}" unless oth.is_a?(String)
      [URI.parse(oth), self]
    end

    def hierarchical?
      !@path.nil?
    end

    def absolute
      absolute?
    end

    def to_str
      to_s
    end

    # The user and the password, "%XX" decoded.
    def decoded_user
      u = user
      u.nil? ? nil : URI.decode_uri_component(u)
    end

    def decoded_password
      pw = password
      pw.nil? ? nil : URI.decode_uri_component(pw)
    end


    # The proxy +env+ names for this URI's scheme ("http_proxy", or
    # "HTTP_PROXY"), as a URI; nil for a loopback host or one +no_proxy+
    # excludes. Under CGI (REQUEST_METHOD set) an HTTP_PROXY is the
    # client's Proxy header, so only the lower-case name counts, or
    # CGI_HTTP_PROXY. CRuby resolves the host with IPSocket.getaddress,
    # which Spinel's socket has no run-time body for: Socket.getaddrinfo.
    def find_proxy(env = ENV)
      raise BadURIError, "relative URI: #{self}" if relative?
      name = "#{@scheme.downcase}_proxy"
      proxy_uri = nil
      if name == "http_proxy" && env.key?("REQUEST_METHOD")
        hits = env.keys.select { |k| k =~ /\Ahttp_proxy\z/i }
        if hits.size == 1
          proxy_uri = env[name] if hits[0] == "http_proxy" && env["HTTP_PROXY"].nil?
        elsif hits.size > 1
          proxy_uri = env[name]
        end
        proxy_uri = env["CGI_HTTP_PROXY"] if proxy_uri.nil?
      elsif name == "http_proxy"
        proxy_uri = env[name]
        if proxy_uri.nil?
          proxy_uri = env["HTTP_PROXY"]
          warn "The environment variable HTTP_PROXY is discouraged.  Please use http_proxy instead." unless proxy_uri.nil?
        end
      else
        proxy_uri = env[name] || env[name.upcase]
      end
      return nil if proxy_uri.nil? || proxy_uri.empty?

      addr = nil
      h = hostname
      unless h.nil?
        begin
          addr = Socket.getaddrinfo(h, nil)[0][3]
        rescue SocketError
          addr = nil
        end
        return nil if !addr.nil? && addr =~ /\A127\.|\A::1\z/
      end
      no_proxy = env["no_proxy"] || env["NO_PROXY"]
      return nil if !no_proxy.nil? && !Generic.use_proxy?(h.to_s, addr, @port, no_proxy)
      URI.parse(proxy_uri)
    end

    # false when +no_proxy+ ("example.com,.local:8080,10.0.0.0/8") names
    # +hostname+, its domain, or a network holding +addr+.
    def self.use_proxy?(hostname, addr, port, no_proxy)
      hostname = hostname.downcase
      dothostname = ".#{hostname}"
      no_proxy.scan(/([^:,\s]+)(?::(\d+))?/) do |m|
        p_host = m[0]
        p_port = m[1]
        next unless p_port.nil? || port == p_port.to_i
        if p_host.start_with?(".")
          return false if hostname.end_with?(p_host.downcase)
        elsif dothostname.end_with?(".#{p_host.downcase}")
          return false
        end
        # CRuby's IPAddr.new(p_host).include?(addr), a host name skipped.
        return false if !addr.nil? && URI.ip_network_include?(p_host, addr)
      end
      true
    end

    def inspect
      "#<#{self.class} #{self}>"
    end

    def check_host(v)
      return false if v.nil?
      raise InvalidURIError, "cannot set host with registry or opaque" unless @opaque.nil?
      raise InvalidComponentError, "bad component(expected host component): #{v}" if !grammar_match?(:HOST, v)
      true
    end

    def check_user(v)
      raise InvalidURIError, "cannot set user with opaque" unless @opaque.nil?
      return if v.nil?
      if !grammar_match?(:USERINFO, v)
        raise InvalidComponentError, "bad component(expected userinfo component or user component): #{v}"
      end
    end

    def check_password(v, user)
      raise InvalidURIError, "cannot set password with opaque" unless @opaque.nil?
      return if v.nil?
      raise InvalidURIError, "password component depends user component" if user.nil?
      raise InvalidComponentError, "bad password component" if !grammar_match?(:USERINFO, v)
    end

    # Whether +v+ is one +name+ of the parser's grammar. Regexp#match, not
    # String#!~: a Regexp read out of the table has no String#!~ at run time.
    def grammar_match?(name, v)
      !parser.regexp[name].match(v.to_s).nil?
    end

    # Equal when of the same class with the same components once normalized.
    def ==(other)
      other.is_a?(Generic) && self.class == other.class && normalize.component_ary == other.normalize.component_ary
    end

    def eql?(other)
      other.is_a?(Generic) && self.class == other.class && component_ary == other.component_ary
    end

    def hash
      component_ary.hash
    end

    protected

    def component_ary
      component.map { |c| component_value(c) }
    end

    # Takes every component of +oth+, a URI of this class (#merge!).
    def replace!(oth)
      raise ArgumentError, "expected #{self.class} object" if self.class != oth.class
      @scheme = oth.scheme
      @userinfo = oth.userinfo
      @host = oth.host
      @port = oth.port
      @path = oth.path
      @query = oth.query
      @fragment = oth.fragment
      @opaque = oth.opaque
    end

    # One component by name: what #select and #== read.
    def component_value(c)
      case c
      when :scheme then @scheme
      when :userinfo then @userinfo
      when :host then @host
      when :port then @port
      when :path then @path
      when :opaque then @opaque
      when :query then @query
      when :fragment then @fragment
      else nil
      end
    end

    private

    # #route_from's path: the dot-segments that climb from the directory of
    # +src+ to +dst+.
    def route_from_path(src, dst)
      return "" if dst == src
      return dst.dup if dst =~ %r{(?:\A|/)\.\.?(?:/|\z)}
      src_path = src.scan(%r{[^/]*/})
      dst_path = dst.scan(%r{[^/]*/?})
      while !dst_path.empty? && dst_path.first == src_path.first
        src_path.shift
        dst_path.shift
      end
      tmp = dst_path.join
      if src_path.empty?
        return "./" if tmp.empty?
        return "./#{tmp}" if dst_path.first.include?(":")
        return tmp
      end
      ("../" * src_path.size) + tmp
    end

    public
  end

  # `URI::HTTPS.build(host: "api.github.com", path: "/x")` -- a URI from
  # its components rather than from a string: keywords, a Hash, or an
  # Array of [userinfo, host, port, path, query, fragment]. A port not
  # given is the scheme's default, which #to_s leaves off, as CRuby does.
  class HTTP < Generic
    def self.default_port
      80
    end

    def self.component
      [:scheme, :userinfo, :host, :port, :path, :query, :fragment]
    end

    def self.build(args = nil, **kw)
      from_components(components_hash(args, kw))
    end

    # What goes on the request line: the path with the query, "/" put in
    # front of a path without one (`URI("http://example.com").request_uri`
    # is "/"); nil for a URI with no path.
    def request_uri
      return nil if @path.nil?
      url = @query.nil? ? @path.dup : "#{@path}?#{@query}"
      url.start_with?("/") ? url : "/#{url}"
    end

    # The host, with ":port" when the port is not the scheme's default --
    # CRuby's URI::HTTP#authority.
    def authority
      @port == default_port ? @host.to_s : "#{@host}:#{@port}"
    end

    # An HTTP URI must name a host: the empty one is refused too.
    def check_host(v)
      ret = super
      raise InvalidComponentError, "bad component(expected host component): #{v}" if ret && v.empty?
      ret
    end

    # "scheme://authority", the web origin a CSP source or a same-origin
    # check compares -- CRuby's URI::HTTP#origin. No userinfo, path, query
    # or fragment.
    def origin
      "#{@scheme}://#{authority}"
    end
  end

  class HTTPS < HTTP
    def self.default_port
      443
    end
  end

  # An FTP URI's path is relative to the login directory: it is kept
  # without the "/" that separates it from the authority, an absolute one
  # is written with "%2F" in its place, and a trailing ";type=a|i|d" is
  # its typecode.
  class FTP < Generic
    attr_reader :typecode

    def self.default_port
      21
    end

    def self.component
      [:scheme, :userinfo, :host, :port, :path, :typecode]
    end

    # Keywords, a Hash or [userinfo, host, port, path, typecode]; the path
    # is relative to the login directory, an absolute one starting "/".
    def self.build(args = nil, **kw)
      h = components_hash(args, kw)
      path = h[:path].to_s
      path = path.start_with?("/") ? "/%2F#{path[1..-1]}" : "/#{path}"
      tc = h[:typecode]
      path = "#{path};type=#{tc}" unless tc.nil?
      from_components(h.merge({ path: path }))
    end

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      raise InvalidURIError if path.nil?
      super
      @typecode = nil
      rel = @path.to_s
      rel = rel[1..-1].to_s if rel.start_with?("/")
      rel = "/#{rel[3..-1]}" if rel.start_with?("%2F")
      ti = rel.index(";type=")
      if ti
        @typecode = rel[(ti + 6)..-1]
        rel = rel[0, ti]
      end
      @path = rel
    end

    def typecode=(v)
      unless v == "a" || v == "i" || v == "d"
        raise InvalidComponentError, "bad typecode(expected a, i, d): #{v}"
      end
      @typecode = v
    end

    # #merge, carrying the reference's typecode over when the result is
    # not this URI. CRuby reads the typecode off the reference even when
    # it is a String or a URI without one, and raises; here it is nil.
    def merge(oth)
      t = super
      t.set_typecode(oth.is_a?(FTP) ? oth.typecode : nil) if t.is_a?(FTP) && self != t
      t
    end

    def to_s
      s = String.new
      s << "#{@scheme}://"
      s << "#{@userinfo}@" unless @userinfo.nil?
      s << @host.to_s
      s << ":#{@port}" if !@port.nil? && @port != default_port
      rel = @path.to_s
      s << "/" << (rel.start_with?("/") ? "%2F#{rel[1..-1]}" : rel)
      s << ";type=#{@typecode}" unless @typecode.nil?
      s << "?#{@query}" unless @query.nil?
      s << "##{@fragment}" unless @fragment.nil?
      s
    end

    protected

    def set_typecode(v)
      @typecode = v
    end

    def replace!(oth)
      super
      @typecode = oth.typecode if oth.is_a?(FTP)
    end

    def component_value(c)
      c == :typecode ? @typecode : super
    end

    public
  end

  # A file URI has a host and a path and nothing else: "localhost" and no
  # host are both "", there is no port, and a userinfo cannot be set.
  class File < Generic
    def self.component
      [:scheme, :host, :path]
    end

    def self.build(args = nil, **kw)
      from_components(components_hash(args, kw))
    end

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      super
      @host = "" if @host.nil? || @host == "localhost"
      @userinfo = nil
      @port = nil
    end

    def self.check_build_userinfo(v)
      raise InvalidURIError, "cannot set userinfo for file URI" unless v.nil?
    end

    def set_host(v)
      @host = v.nil? || v == "localhost" ? "" : v
    end

    def set_port(v)
    end

    def set_userinfo(v)
    end

    def set_user(v)
    end

    def set_password(v)
    end

    def check_userinfo(user)
      raise InvalidURIError, "cannot set userinfo for file URI"
    end

    def check_user(user)
      raise InvalidURIError, "cannot set user for file URI"
    end

    def check_password(user, _ = nil)
      raise InvalidURIError, "cannot set password for file URI"
    end

    def userinfo=(v)
      return nil if v.nil?
      raise InvalidURIError, "cannot set userinfo for file URI"
    end

    def user=(v)
      raise InvalidURIError, "cannot set user for file URI"
    end

    def password=(v)
      raise InvalidURIError, "cannot set password for file URI"
    end
  end

  class WS < Generic
    def self.default_port
      80
    end

    def self.component
      [:scheme, :userinfo, :host, :port, :path, :query]
    end

    def self.build(args = nil, **kw)
      from_components(components_hash(args, kw))
    end

    # What goes on the request line: the path with the query, "/" put in
    # front of a path without one (`URI("http://example.com").request_uri`
    # is "/"); nil for a URI with no path.
    def request_uri
      return nil if @path.nil?
      url = @query.nil? ? @path.dup : "#{@path}?#{@query}"
      url.start_with?("/") ? url : "/#{url}"
    end
  end

  class WSS < WS
    def self.default_port
      443
    end
  end

  # `mailto:to?hname=hvalue&...`: the addresses before the "?", and the
  # headers after it as [name, value] pairs, both as written.
  class MailTo < Generic
    # CRuby's, with its `\g<hfield>` call spelled out.
    HFIELD = "(?:%\\h\\h|[!$'-.0-;@-Z_a-z~])*=(?:%\\h\\h|[!$'-.0-;@-Z_a-z~])*"
    HEADER_REGEXP = Regexp.new("\\A#{HFIELD}(?:&#{HFIELD})*\\z")
    EMAIL_REGEXP = /\A[a-zA-Z0-9.!\#$%&'*+\/=?^_`{|}~-]+@[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*\z/

    def self.component
      [:scheme, :to, :headers]
    end

    # Keywords, a Hash or [to, headers]: +to+ a String or an Array of
    # addresses, +headers+ a String, [name, value] pairs or a Hash.
    def self.build(args = nil, **kw)
      h = components_hash(args, kw)
      to = h[:to]
      opaque = if to.is_a?(Array)
                 to.join(",")
               elsif to.is_a?(String)
                 to.dup
               else
                 +""
               end
      hs = h[:headers]
      unless hs.nil?
        query = if hs.is_a?(Array)
                  hs.map { |x| x.is_a?(Array) ? "#{x[0]}=#{x[1..-1].join}" : x.to_s }.join("&")
                elsif hs.is_a?(Hash)
                  hs.map { |k, v| "#{k}=#{v}" }.join("&")
                else
                  hs.to_s
                end
        opaque << "?" << query unless query.empty?
      end
      u = new("mailto", nil, nil, nil, nil, nil, nil)
      u.set_opaque(opaque)
      u.check_mailto!
      to_part, header_part = opaque.split("?", 2)
      u.to = to_part
      u.headers = header_part
      u
    end

    # CRuby's MailTo#initialize checks: a query alone is the headers, there
    # must be an opaque part, and its addresses must read as addresses.
    def check_mailto!
      @opaque = "?#{@query}" if @opaque.nil? && !@query.nil?
      raise InvalidComponentError, "missing opaque part for mailto URL" if @opaque.nil?
      if to !~ /\A(?:[^@,;]+@[^@,;]+(?:\z|[,;]))*\z/
        raise InvalidComponentError, "unrecognised opaque part for mailtoURL: #{@opaque}"
      end
      self
    end

    def to
      o = @opaque.to_s
      i = o.index("?")
      i ? o[0, i] : o
    end

    def headers
      o = @opaque.to_s
      i = o.index("?")
      return [] unless i
      o[(i + 1)..-1].to_s.split("&").map { |h| h.split("=", 2) }
    end

    # The addresses, each a "%XX"-escaped addr-spec, "," or ";" between them.
    def to=(v)
      unless v.nil? || v.empty?
        v.split(/[,;]/).each do |addr|
          if addr !~ /\A(?:%\h\h|[!$&-.0-;=@-Z_a-z~])*\z/
            raise InvalidComponentError, "an address in 'to' is invalid as URI #{addr.dump}"
          end
          if URI.decode_uri_component(addr) !~ EMAIL_REGEXP
            raise InvalidComponentError, "an address in 'to' is invalid as uri-escaped addr-spec #{addr.dump}"
          end
        end
      end
      rebuild_opaque(v.to_s, headers_query)
      v
    end

    # "name=value&..." -- the headers part after the "?".
    def headers=(v)
      if !v.nil? && !v.empty? && v !~ HEADER_REGEXP
        raise InvalidComponentError, "bad component(expected opaque component): #{v}"
      end
      rebuild_opaque(to, v.to_s)
      v
    end

    # The message the URI describes, as RFC 822 text: "To:" (with any "to"
    # header added), the other headers capitalized, a blank line, the body.
    def to_mailtext
      to_line = URI.decode_www_form_component(to)
      head = String.new
      body = ""
      headers.each do |x|
        case x[0]
        when "body"
          body = URI.decode_www_form_component(x[1].to_s)
        when "to"
          to_line << ", " + URI.decode_www_form_component(x[1].to_s)
        else
          head << URI.decode_www_form_component(x[0].to_s).capitalize + ": " + URI.decode_www_form_component(x[1].to_s) + "\n"
        end
      end
      "To: #{to_line}\n#{head}\n#{body}\n"
    end

    def to_rfc822text
      to_mailtext
    end

    protected

    def component_value(c)
      case c
      when :to then to
      when :headers then headers
      else super
      end
    end

    private

    def headers_query
      o = @opaque.to_s
      i = o.index("?")
      i ? o[(i + 1)..-1].to_s : ""
    end

    def rebuild_opaque(to, query)
      @opaque = query.empty? ? to : "#{to}?#{query}"
    end

    public
  end

  # `ldap://host:port/dn?attributes?scope?filter?extensions`: the
  # distinguished name is the path after its "/", the rest the "?"-split
  # query, and each has a reader and a writer that rewrites the URI.
  class LDAP < Generic
    SCOPE_ONE = "one"
    SCOPE_SUB = "sub"
    SCOPE_BASE = "base"
    SCOPE = [SCOPE_ONE, SCOPE_SUB, SCOPE_BASE]

    attr_reader :dn, :attributes, :scope, :filter, :extensions

    def self.default_port
      389
    end

    def self.component
      [:scheme, :host, :port, :dn, :attributes, :scope, :filter, :extensions]
    end

    # Keywords, a Hash or [host, port, dn, attributes, scope, filter,
    # extensions]: the query drops the trailing parts not given.
    def self.build(args = nil, **kw)
      h = components_hash(args, kw)
      h = h.merge({ path: h[:dn] }) unless h[:dn].nil?
      query = []
      [h[:extensions], h[:filter], h[:scope], h[:attributes]].each do |x|
        next if x.nil? && query.empty?
        query.unshift(x)
      end
      from_components(h.merge({ query: query.join("?") }))
    end

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      super
      raise InvalidURIError, "bad LDAP URL" unless @fragment.nil?
      raise InvalidURIError, "bad LDAP URL" if @path.nil?
      @dn = @path[1..-1].to_s
      @attributes = nil
      @scope = nil
      @filter = nil
      @extensions = nil
      unless @query.nil?
        parts = @query.split("?")
        @attributes = parts[0] if !parts[0].nil? && !parts[0].empty?
        @scope = parts[1] if !parts[1].nil? && !parts[1].empty?
        @filter = parts[2] if !parts[2].nil? && !parts[2].empty?
        @extensions = parts[3] if !parts[3].nil? && !parts[3].empty?
      end
    end

    def dn=(v)
      @dn = v
      build_path_query
      v
    end

    def attributes=(v)
      @attributes = v
      build_path_query
      v
    end

    def scope=(v)
      @scope = v
      build_path_query
      v
    end

    def filter=(v)
      @filter = v
      build_path_query
      v
    end

    def extensions=(v)
      @extensions = v
      build_path_query
      v
    end

    def hierarchical?
      false
    end

    protected

    def component_value(c)
      case c
      when :dn then @dn
      when :attributes then @attributes
      when :scope then @scope
      when :filter then @filter
      when :extensions then @extensions
      else super
      end
    end

    private

    def build_path_query
      @path = "/#{@dn}"
      query = []
      [@extensions, @filter, @scope, @attributes].each do |x|
        next if x.nil? && query.empty?
        query.unshift(x)
      end
      @query = query.join("?")
    end

    public
  end

  class LDAPS < LDAP
    def self.default_port
      636
    end
  end

  # The scheme registry URI.parse and URI.for pick a class from: the
  # bundled schemes, and what URI.register_scheme adds, by upper-case name.
  SCHEMES = {
    "WS" => WS, "WSS" => WSS, "HTTP" => HTTP, "HTTPS" => HTTPS, "LDAP" => LDAP,
    "FTP" => FTP, "FILE" => File, "LDAPS" => LDAPS, "MAILTO" => MailTo,
  }

  def self.register_scheme(scheme, klass)
    SCHEMES[scheme.to_s.upcase] = klass
  end

  def self.scheme_list
    SCHEMES.dup
  end

  # The class a scheme's URI is, +default+ for one not registered.
  def self.scheme_class(scheme, default = Generic)
    return default if scheme.nil? || !scheme.ascii_only?
    k = SCHEMES[scheme.upcase]
    k.nil? ? default : k
  end

  # A URI of +scheme+'s class from the nine components URI.split answers,
  # as CRuby's URI.for builds it: the scheme in lower case, a port String
  # as an Integer ("" as none), a registry refused, the empty path for a
  # URI with neither a path nor an opaque part.
  def self.for(scheme, userinfo = nil, host = nil, port = nil, registry = nil, path = nil, opaque = nil, query = nil, fragment = nil, parser = nil, default: Generic)
    klass = scheme_class(scheme, default)
    s = scheme.nil? ? nil : scheme.downcase
    unless registry.nil?
      raise InvalidURIError, "the scheme #{s} does not accept registry part: #{registry} (or bad hostname?)"
    end
    pn = nil
    if port.is_a?(Integer)
      pn = port
    elsif !port.nil? && !port.empty?
      pn = port.to_i
    end
    path = "" if path.nil? && opaque.nil?
    u = klass.new(s, userinfo, host, pn, path, query, fragment)
    u.set_opaque(opaque) unless opaque.nil?
    u.parser = parser unless parser.nil? || parser.equal?(RFC3986_PARSER)
    u.check_mailto! if u.is_a?(MailTo)
    u
  end

  def self.from_split(parts, parser = nil)
    self.for(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7], parts[8], parser)
  end

  # Percent-encode one www-form component: everything but the unreserved set,
  # with a space as "+", which is what CRuby does here (and what differs from
  # a path escape).
  def self.encode_www_form_component(str)
    out = String.new
    str.to_s.each_char do |ch|
      if ch =~ /\A[A-Za-z0-9\*\-\.\_]\z/
        out << ch
      elsif ch == " "
        out << "+"
      else
        ch.bytes.each { |b| out << format("%%%02X", b) }
      end
    end
    out
  end

  # The value of one hex digit character, or nil for anything else.
  def self.hex_digit(c)
    return nil unless c
    o = c.ord
    return o - 48 if o >= 48 && o <= 57
    return o - 55 if o >= 65 && o <= 70
    return o - 87 if o >= 97 && o <= 102
    nil
  end

  def self.decode_www_form_component(str, enc = Encoding::UTF_8)
    percent_decode(str, enc, true)
  end

  # `encode_www_form_component` with a space as "%20", as CRuby's
  # URI.encode_uri_component answers.
  def self.encode_uri_component(str)
    encode_www_form_component(str).gsub("+", "%20")
  end

  # `decode_www_form_component` with "+" left as it is.
  def self.decode_uri_component(str, enc = Encoding::UTF_8)
    percent_decode(str, enc, false)
  end

  # Each "%" and two hex digits as that byte, and with +plus+ each "+" as a
  # space; any other "%" is CRuby's ArgumentError.
  def self.percent_decode(str, enc, plus)
    out = String.new
    s = str.to_s
    i = 0
    while i < s.length
      ch = s[i]
      if plus && ch == "+"
        out << " "
        i += 1
      elsif ch == "%"
        # a % not followed by two hex digits is CRuby's ArgumentError
        h1 = hex_digit(s[i + 1])
        h2 = hex_digit(s[i + 2])
        raise ArgumentError, "invalid %-encoding (#{s})" unless h1 && h2
        out << (h1 * 16 + h2).chr
        i += 3
      else
        out << ch
        i += 1
      end
    end
    out.force_encoding(enc)
  end

  # The www-form decoding of one key or value: "+" is a space and a "%" with two
  # hex digits is that byte. Unlike decode_www_form_component, a "%" that is not
  # followed by two hex digits is left as it is, not an error.
  def self.decode_www_form_lenient(s, enc = Encoding::UTF_8)
    out = String.new
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "+"
        out << " "
        i += 1
      elsif ch == "%" && hex_digit(s[i + 1]) && hex_digit(s[i + 2])
        out << (hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2])).chr
        i += 3
      else
        out << ch
        i += 1
      end
    end
    out.force_encoding(enc).scrub
  end

  # `URI.decode_www_form("a=1&b=x+y")` -> [["a", "1"], ["b", "x y"]]. Keys and
  # values take the encoding argument, UTF-8 by default. `use__charset_` is not
  # supported.
  def self.decode_www_form(str, enc = Encoding::UTF_8, separator: "&", use__charset_: false, isindex: false)
    raise ArgumentError, "the input of URI.decode_www_form must be ASCII only string" unless str.ascii_only?
    raise NotImplementedError, "URI.decode_www_form: use__charset_ is not supported" if use__charset_
    raise NotImplementedError, "URI.decode_www_form: an empty separator is not supported" if separator.empty?
    ary = []
    return ary if str.empty?
    pos = 0
    n = str.length
    sl = separator.length
    while pos < n
      e = str.index(separator, pos)
      if e
        piece = str[pos, e - pos]
        pos = e + sl
      else
        piece = str[pos, n - pos]
        pos = n
      end
      eq = piece.index("=")
      key = eq ? piece[0, eq] : piece
      val = eq ? piece[eq + 1, piece.length - eq - 1] : ""
      if isindex
        if eq.nil?
          val = key
          key = ""
        end
        isindex = false
      end
      ary << [decode_www_form_lenient(key, enc), decode_www_form_lenient(val, enc)]
    end
    ary
  end

  # `URI.encode_www_form({"a" => 1, "b" => "x y"})` -> "a=1&b=x+y"
  def self.encode_www_form(pairs)
    parts = []
    pairs.each do |k, v|
      key = encode_www_form_component(k)
      if v.nil?
        parts << key
      elsif v.respond_to?(:to_ary)
        values = []
        v.to_ary.each do |item|
          values << (item.nil? ? "" : "#{key}=#{encode_www_form_component(item)}")
        end
        parts << values.join("&")
      else
        parts << "#{key}=#{encode_www_form_component(v)}"
      end
    end
    parts.join("&")
  end

  # RFC 2396's grammar as regexp sources: the strings CRuby's
  # RFC2396_Parser builds its patterns from, and which a server reads to
  # match what a request carries (webrick matches a Host header with
  # `URI::RFC2396_Parser.new.pattern.fetch(:HOST)`). PATTERN holds each
  # entry of the default table, as CRuby fills it in after the class.
  module RFC2396_REGEXP
    module PATTERN
      ALPHA = "a-zA-Z"
      ALNUM = "#{ALPHA}\\d"
      HEX = "a-fA-F\\d"
      ESCAPED = "%[#{HEX}]{2}"
      UNRESERVED = "\\-_.!~*'()#{ALNUM}"
      RESERVED = ";/?:@&=+$,\\[\\]"
      DOMLABEL = "(?:[#{ALNUM}](?:[-#{ALNUM}]*[#{ALNUM}])?)"
      TOPLABEL = "(?:[#{ALPHA}](?:[-#{ALNUM}]*[#{ALNUM}])?)"
      HOSTNAME = "(?:#{DOMLABEL}\\.)*#{TOPLABEL}\\.?"
      URIC = "(?:[#{UNRESERVED}#{RESERVED}]|#{ESCAPED})"
      URIC_NO_SLASH = "(?:[#{UNRESERVED};?:@&=+$,]|#{ESCAPED})"
      QUERY = "#{URIC}*"
      FRAGMENT = "#{URIC}*"
      IPV4ADDR = "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}"
      IPV6ADDR = "(?:(?:[#{HEX}]{1,4}:)*(?:[#{HEX}]{1,4}|#{IPV4ADDR})|(?:(?:[#{HEX}]{1,4}:)*[#{HEX}]{1,4})?::(?:(?:[#{HEX}]{1,4}:)*(?:[#{HEX}]{1,4}|#{IPV4ADDR}))?)"
      IPV6REF = "\\[#{IPV6ADDR}\\]"
      # The table's HOST is built on its laxer hostname, not HOSTNAME above.
      HOST = "(?:(?:[a-zA-Z0-9\\-.]|%\\h\\h)+|#{IPV4ADDR}|#{IPV6REF})"
      PORT = "\\d*"
      HOSTPORT = "#{HOST}(?::#{PORT})?"
      USERINFO = "(?:[#{UNRESERVED};:&=+$,]|#{ESCAPED})*"
      PATH_SEGMENTS = "(?:[#{UNRESERVED}:@&=+$,]|#{ESCAPED})*(?:;(?:[#{UNRESERVED}:@&=+$,]|#{ESCAPED})*)*(?:/(?:[#{UNRESERVED}:@&=+$,]|#{ESCAPED})*(?:;(?:[#{UNRESERVED}:@&=+$,]|#{ESCAPED})*)*)*"
      REG_NAME = "(?:[#{UNRESERVED}$,;:@&=+]|#{ESCAPED})+"
      REL_SEGMENT = "(?:[#{UNRESERVED};@&=+$,]|#{ESCAPED})+"
      SCHEME = "[#{ALPHA}][\\-+.#{ALPHA}\\d]*"
      ABS_PATH = "/#{PATH_SEGMENTS}"
      REL_PATH = "#{REL_SEGMENT}(?:#{ABS_PATH})?"
      NET_PATH = "//(?:(?:#{USERINFO}@)?#{HOSTPORT}|#{REG_NAME})(?:#{ABS_PATH})?"
      HIER_PART = "(?:#{NET_PATH}|#{ABS_PATH})(?:\\?(?:#{QUERY}))?"
      OPAQUE_PART = "#{URIC_NO_SLASH}#{URIC}*"
      ABS_URI = "#{SCHEME}:(?:#{HIER_PART}|#{OPAQUE_PART})"
      REL_URI = "(?:#{NET_PATH}|#{ABS_PATH}|#{REL_PATH})(?:\\?#{QUERY})?"
      URI_REF = "(?:#{ABS_URI}|#{REL_URI})?(?:##{FRAGMENT})?"
    end
  end

  # CRuby's RFC 2396 parser: the regexp-driven #split and #parse of the
  # older grammar, #escape / #unescape (the URI.escape the RFC 3986
  # parser delegates to), #extract and #make_regexp, and the #pattern and
  # #regexp tables, each pattern overridable through +opts+.
  class RFC2396_Parser
    include RFC2396_REGEXP

    attr_reader :pattern, :regexp

    def initialize(opts = {})
      @pattern = initialize_pattern(opts)
      @regexp = initialize_regexp(@pattern)
    end

    def split(uri)
      scheme = nil
      userinfo = nil
      host = nil
      port = nil
      registry = nil
      path = nil
      opaque = nil
      query = nil
      fragment = nil
      if uri == ""
        # an empty reference: just the empty path
      elsif (m = @regexp[:ABS_URI].match(uri))
        scheme = m[1]
        opaque = m[2]
        userinfo = m[3]
        host = m[4]
        port = m[5]
        registry = m[6]
        path = m[7]
        query = m[8]
        fragment = m[9]
        raise InvalidURIError, "bad URI (absolute but no scheme): #{uri}" if scheme.nil?
        if opaque.nil? && path.nil? && host.nil? && registry.nil?
          raise InvalidURIError, "bad URI (absolute but no path): #{uri}"
        end
      elsif (m = @regexp[:REL_URI].match(uri))
        userinfo = m[1]
        host = m[2]
        port = m[3]
        registry = m[4]
        rel_segment = m[5]
        abs_path = m[6]
        query = m[7]
        fragment = m[8]
        if !rel_segment.nil? && !abs_path.nil?
          path = rel_segment + abs_path
        elsif !rel_segment.nil?
          path = rel_segment
        elsif !abs_path.nil?
          path = abs_path
        end
      else
        raise InvalidURIError, "bad URI (is not URI?): #{uri}"
      end
      path = "" if path.nil? && opaque.nil?
      [scheme, userinfo, host, port, registry, path, opaque, query, fragment]
    end

    def parse(uri)
      URI.from_split(split(uri), self)
    end

    def join(*uris)
      u = convert_to_uri(uris[0])
      uris[1..-1].each { |r| u = u.merge(r) }
      u
    end

    # Each absolute URI in +str+, each yielded when a block is given.
    def extract(str, schemes = nil)
      re = make_regexp(schemes)
      found = []
      pos = 0
      # The rest of +str+ matched, not #match(str, pos): a Regexp read out of the table has no two-argument #match at run time.
      while pos <= str.length && (m = re.match(str[pos..-1]))
        found << m[0]
        pos += m.end(0)
        pos += 1 if m[0].empty?
      end
      if block_given?
        found.each { |u| yield u }
        nil
      else
        found
      end
    end

    # The absolute-URI pattern, or one that also requires one of +schemes+.
    def make_regexp(schemes = nil)
      return @regexp[:ABS_URI_REF] if schemes.nil?
      Regexp.new("(?=(?i:#{Regexp.union(*schemes).source}):)#{@pattern[:X_ABS_URI]}", Regexp::EXTENDED)
    end

    # Each byte of each match of +unsafe+ (a Regexp, or a String of the
    # characters to escape) as "%XX".
    def escape(str, unsafe = @regexp[:UNSAFE])
      unsafe = Regexp.new("[#{Regexp.quote(unsafe)}]") unless unsafe.is_a?(Regexp)
      str.gsub(unsafe) { |us| us.bytes.map { |b| format("%%%02X", b) }.join }.force_encoding(Encoding::US_ASCII)
    end

    # Each match of +escaped+ ("%XX") as its byte, in the string's own
    # encoding (UTF-8 for a US-ASCII one).
    def unescape(str, escaped = @regexp[:ESCAPED])
      enc = str.encoding
      enc = Encoding::UTF_8 if enc == Encoding::US_ASCII
      str.gsub(escaped) { |e| e[1, 2].to_i(16).chr.force_encoding(enc) }
    end

    private

    def initialize_pattern(opts = {})
      escaped = opts[:ESCAPED] || PATTERN::ESCAPED
      unreserved = opts[:UNRESERVED] || PATTERN::UNRESERVED
      reserved = opts[:RESERVED] || PATTERN::RESERVED
      p_domlabel = opts[:DOMLABEL] || PATTERN::DOMLABEL
      p_toplabel = opts[:TOPLABEL] || PATTERN::TOPLABEL
      hostname = opts[:HOSTNAME]
      uric = "(?:[#{unreserved}#{reserved}]|#{escaped})"
      uric_no_slash = "(?:[#{unreserved};?:@&=+$,]|#{escaped})"
      query = "#{uric}*"
      fragment = "#{uric}*"
      hostname = "(?:[a-zA-Z0-9\\-.]|%\\h\\h)+" if hostname.nil?
      p_hostname = hostname
      ipv4addr = "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}"
      hex4 = "[#{PATTERN::HEX}]{1,4}"
      lastpart = "(?:#{hex4}|#{ipv4addr})"
      hexseq1 = "(?:#{hex4}:)*#{hex4}"
      hexseq2 = "(?:#{hex4}:)*#{lastpart}"
      ipv6addr = "(?:#{hexseq2}|(?:#{hexseq1})?::(?:#{hexseq2})?)"
      ipv6ref = "\\[#{ipv6addr}\\]"
      host = "(?:#{hostname}|#{ipv4addr}|#{ipv6ref})"
      port = "\\d*"
      hostport = "#{host}(?::#{port})?"
      userinfo = "(?:[#{unreserved};:&=+$,]|#{escaped})*"
      pchar = "(?:[#{unreserved}:@&=+$,]|#{escaped})"
      param = "#{pchar}*"
      segment = "#{pchar}*(?:;#{param})*"
      path_segments = "#{segment}(?:/#{segment})*"
      server = "(?:#{userinfo}@)?#{hostport}"
      reg_name = "(?:[#{unreserved}$,;:@&=+]|#{escaped})+"
      authority = "(?:#{server}|#{reg_name})"
      rel_segment = "(?:[#{unreserved};@&=+$,]|#{escaped})+"
      scheme = "[#{PATTERN::ALPHA}][\\-+.#{PATTERN::ALPHA}\\d]*"
      abs_path = "/#{path_segments}"
      rel_path = "#{rel_segment}(?:#{abs_path})?"
      net_path = "//#{authority}(?:#{abs_path})?"
      hier_part = "(?:#{net_path}|#{abs_path})(?:\\?(?:#{query}))?"
      opaque_part = "#{uric_no_slash}#{uric}*"
      abs_uri = "#{scheme}:(?:#{hier_part}|#{opaque_part})"
      rel_uri = "(?:#{net_path}|#{abs_path}|#{rel_path})(?:\\?#{query})?"
      p_uri_ref = "(?:#{abs_uri}|#{rel_uri})?(?:##{fragment})?"
      p_x_abs_uri = "
        (#{scheme}):                           (?# 1: scheme)
        (?:
           (#{opaque_part})                    (?# 2: opaque)
        |
           (?:(?:
             //(?:
                 (?:(?:(#{userinfo})@)?        (?# 3: userinfo)
                   (?:(#{host})(?::(\\d*))?))? (?# 4: host, 5: port)
               |
                 (#{reg_name})                 (?# 6: registry)
               )
             |
             (?!//))                           (?# XXX: '//' is the mark for hostport)
             (#{abs_path})?                    (?# 7: path)
           )(?:\\?(#{query}))?                 (?# 8: query)
        )
        (?:\\#(#{fragment}))?                  (?# 9: fragment)
      "
      p_x_rel_uri = "
        (?:
          (?:
            //
            (?:
              (?:(#{userinfo})@)?       (?# 1: userinfo)
                (#{host})?(?::(\\d*))?  (?# 2: host, 3: port)
            |
              (#{reg_name})             (?# 4: registry)
            )
          )
        |
          (#{rel_segment})              (?# 5: rel_segment)
        )?
        (#{abs_path})?                  (?# 6: abs_path)
        (?:\\?(#{query}))?              (?# 7: query)
        (?:\\#(#{fragment}))?           (?# 8: fragment)
      "
      {
        ESCAPED: escaped,
        UNRESERVED: unreserved,
        RESERVED: reserved,
        DOMLABEL: p_domlabel,
        TOPLABEL: p_toplabel,
        HOSTNAME: p_hostname,
        URIC: uric,
        URIC_NO_SLASH: uric_no_slash,
        QUERY: query,
        FRAGMENT: fragment,
        IPV4ADDR: ipv4addr,
        IPV6ADDR: ipv6addr,
        IPV6REF: ipv6ref,
        HOST: host,
        PORT: port,
        HOSTPORT: hostport,
        USERINFO: userinfo,
        PATH_SEGMENTS: path_segments,
        REG_NAME: reg_name,
        REL_SEGMENT: rel_segment,
        SCHEME: scheme,
        ABS_PATH: abs_path,
        REL_PATH: rel_path,
        NET_PATH: net_path,
        HIER_PART: hier_part,
        OPAQUE_PART: opaque_part,
        ABS_URI: abs_uri,
        REL_URI: rel_uri,
        URI_REF: p_uri_ref,
        X_ABS_URI: p_x_abs_uri,
        X_REL_URI: p_x_rel_uri,
      }
    end

    def initialize_regexp(pattern)
      {
        ABS_URI: Regexp.new('\A\s*+' + pattern[:X_ABS_URI] + '\s*\z', Regexp::EXTENDED),
        REL_URI: Regexp.new('\A\s*+' + pattern[:X_REL_URI] + '\s*\z', Regexp::EXTENDED),
        URI_REF: Regexp.new(pattern[:URI_REF]),
        ABS_URI_REF: Regexp.new(pattern[:X_ABS_URI], Regexp::EXTENDED),
        REL_URI_REF: Regexp.new(pattern[:X_REL_URI], Regexp::EXTENDED),
        ESCAPED: Regexp.new(pattern[:ESCAPED]),
        UNSAFE: Regexp.new("[^#{pattern[:UNRESERVED]}#{pattern[:RESERVED]}]"),
        SCHEME: Regexp.new("\\A#{pattern[:SCHEME]}\\z"),
        USERINFO: Regexp.new("\\A#{pattern[:USERINFO]}\\z"),
        HOST: Regexp.new("\\A#{pattern[:HOST]}\\z"),
        PORT: Regexp.new("\\A#{pattern[:PORT]}\\z"),
        OPAQUE: Regexp.new("\\A#{pattern[:OPAQUE_PART]}\\z"),
        REGISTRY: Regexp.new("\\A#{pattern[:REG_NAME]}\\z"),
        ABS_PATH: Regexp.new("\\A#{pattern[:ABS_PATH]}\\z"),
        REL_PATH: Regexp.new("\\A#{pattern[:REL_PATH]}\\z"),
        QUERY: Regexp.new("\\A#{pattern[:QUERY]}\\z"),
        FRAGMENT: Regexp.new("\\A#{pattern[:FRAGMENT]}\\z"),
      }
    end

    def convert_to_uri(uri)
      URI.for_merge(uri)
    end
  end

  # The parser URI.parse is (URI::DEFAULT_PARSER), CRuby's: RFC 3986's
  # grammar as one regexp for a URI and one for a relative reference,
  # which #split matches to answer the nine components. #extract,
  # #make_regexp, #escape and #unescape hand over to the RFC 2396 parser,
  # as CRuby's do.
  class RFC3986_Parser
    HOST = %r[
      (?<IP-literal>\[(?:
          (?<IPv6address>
            (?:\h{1,4}:){6}
            (?<ls32>\h{1,4}:\h{1,4}
            | (?<IPv4address>(?<dec-octet>[1-9]\d|1\d{2}|2[0-4]\d|25[0-5]|\d)
                \.\g<dec-octet>\.\g<dec-octet>\.\g<dec-octet>)
            )
          | ::(?:\h{1,4}:){5}\g<ls32>
          | \h{1,4}?::(?:\h{1,4}:){4}\g<ls32>
          | (?:(?:\h{1,4}:)?\h{1,4})?::(?:\h{1,4}:){3}\g<ls32>
          | (?:(?:\h{1,4}:){,2}\h{1,4})?::(?:\h{1,4}:){2}\g<ls32>
          | (?:(?:\h{1,4}:){,3}\h{1,4})?::\h{1,4}:\g<ls32>
          | (?:(?:\h{1,4}:){,4}\h{1,4})?::\g<ls32>
          | (?:(?:\h{1,4}:){,5}\h{1,4})?::\h{1,4}
          | (?:(?:\h{1,4}:){,6}\h{1,4})?::
          )
        | (?<IPvFuture>v\h++\.[!$&-.0-9:;=A-Z_a-z~]++)
        )\])
    | \g<IPv4address>
    | (?<reg-name>(?:%\h\h|[!$&-.0-9;=A-Z_a-z~])*+)
    ]x
    USERINFO = /(?:%\h\h|[!$&-.0-9:;=A-Z_a-z~])*+/
    SCHEME = %r[[A-Za-z][+\-.0-9A-Za-z]*+].source
    SEG = %r[(?:%\h\h|[!$&-.0-9:;=@A-Z_a-z~/])].source
    SEG_NC = %r[(?:%\h\h|[!$&-.0-9;=@A-Z_a-z~])].source
    FRAGMENT = %r[(?:%\h\h|[!$&-.0-9:;=@A-Z_a-z~/?])*+].source
    RFC3986_URI = %r[\A
    (?<seg>#{SEG}){0}
    (?<URI>
      (?<scheme>#{SCHEME}):
      (?<hier-part>//
        (?<authority>
          (?:(?<userinfo>#{USERINFO.source})@)?
          (?<host>#{HOST.source.delete(" \n")})
          (?::(?<port>\d*+))?
        )
        (?<path-abempty>(?:/\g<seg>*+)?)
      | (?<path-absolute>/((?!/)\g<seg>++)?)
      | (?<path-rootless>(?!/)\g<seg>++)
      | (?<path-empty>)
      )
      (?:\?(?<query>[^\#]*+))?
      (?:\#(?<fragment>#{FRAGMENT}))?
    )\z]x
    RFC3986_relative_ref = %r[\A
    (?<seg>#{SEG}){0}
    (?<relative-ref>
      (?<relative-part>//
        (?<authority>
          (?:(?<userinfo>#{USERINFO.source})@)?
          (?<host>#{HOST.source.delete(" \n")}(?<!/))?
          (?::(?<port>\d*+))?
        )
        (?<path-abempty>(?:/\g<seg>*+)?)
      | (?<path-absolute>/\g<seg>*+)
      | (?<path-noscheme>#{SEG_NC}++(?:/\g<seg>*+)?)
      | (?<path-empty>)
      )
      (?:\?(?<query>[^#]*+))?
      (?:\#(?<fragment>#{FRAGMENT}))?
    )\z]x

    attr_reader :regexp

    def initialize
      @regexp = default_regexp
    end

    def split(uri)
      unless uri.is_a?(String)
        raise InvalidURIError, "bad URI (is not URI?): #{uri.inspect}" unless uri.is_a?(Generic)
        uri = uri.to_s
      end
      raise InvalidURIError, "URI must be ascii only #{uri.dump}" unless uri.ascii_only?
      if (m = RFC3986_URI.match(uri))
        query = m["query"]
        scheme = m["scheme"]
        opaque = m["path-rootless"]
        if opaque
          opaque = "#{opaque}?#{query}" if query
          [scheme, nil, nil, nil, nil, nil, opaque, nil, m["fragment"]]
        else
          path = m["path-abempty"] || m["path-absolute"] || m["path-empty"]
          [scheme, m["userinfo"], m["host"], m["port"], nil, path, nil, query, m["fragment"]]
        end
      elsif (m = RFC3986_relative_ref.match(uri))
        path = m["path-abempty"] || m["path-absolute"] || m["path-noscheme"] || m["path-empty"]
        [nil, m["userinfo"], m["host"], m["port"], nil, path, nil, m["query"], m["fragment"]]
      else
        raise InvalidURIError, "bad URI (is not URI?): #{uri.inspect}"
      end
    end

    def parse(uri)
      URI.from_split(split(uri))
    end

    def join(*uris)
      u = URI.for_merge(uris[0])
      uris[1..-1].each { |r| u = u.merge(r) }
      u
    end

    def extract(str, schemes = nil)
      warn "URI::RFC3986_PARSER.extract is obsolete. Use URI::RFC2396_PARSER.extract explicitly." if $VERBOSE
      if block_given?
        RFC2396_PARSER.extract(str, schemes) { |u| yield u }
      else
        RFC2396_PARSER.extract(str, schemes)
      end
    end

    def make_regexp(schemes = nil)
      warn "URI::RFC3986_PARSER.make_regexp is obsolete. Use URI::RFC2396_PARSER.make_regexp explicitly." if $VERBOSE
      RFC2396_PARSER.make_regexp(schemes)
    end

    def escape(str, unsafe = nil)
      warn "URI::RFC3986_PARSER.escape is obsolete. Use URI::RFC2396_PARSER.escape explicitly." if $VERBOSE
      unsafe ? RFC2396_PARSER.escape(str, unsafe) : RFC2396_PARSER.escape(str)
    end

    def unescape(str, escaped = nil)
      warn "URI::RFC3986_PARSER.unescape is obsolete. Use URI::RFC2396_PARSER.unescape explicitly." if $VERBOSE
      escaped ? RFC2396_PARSER.unescape(str, escaped) : RFC2396_PARSER.unescape(str)
    end

    private

    def default_regexp
      {
        SCHEME: %r[\A#{SCHEME}\z],
        USERINFO: %r[\A#{USERINFO}\z],
        HOST: %r[\A#{HOST}\z],
        ABS_PATH: %r[\A/#{SEG}*+\z],
        REL_PATH: %r[\A(?!/)#{SEG}++\z],
        QUERY: %r[\A(?:%\h\h|[!$&-.0-9:;=@A-Z_a-z~/?])*+\z],
        FRAGMENT: %r[\A#{FRAGMENT}\z],
        OPAQUE: %r[\A(?:[^/].*)?\z],
        PORT: /\A[\x09\x0a\x0c\x0d ]*+\d*[\x09\x0a\x0c\x0d ]*\z/,
      }
    end
  end

  RFC2396_PARSER = RFC2396_Parser.new
  RFC3986_PARSER = RFC3986_Parser.new
  DEFAULT_PARSER = RFC3986_PARSER
  PARSER = RFC3986_PARSER
  Parser = RFC3986_Parser
  REGEXP = RFC2396_REGEXP

  # The bytes of an IPv4 (4) or IPv6 (16) address, nil for anything else.
  # Not IPAddr: requiring it here would load it into every program using
  # uri, and an app's own IPAddr (a Rails runtime shim) then collides.
  def self.ip_bytes(s)
    if (m = /\A(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\z/.match(s))
      b = [m[1].to_i, m[2].to_i, m[3].to_i, m[4].to_i]
      return b.any? { |o| o > 255 } ? nil : b
    end
    return nil unless s.include?(":") && s =~ /\A[0-9A-Fa-f:.]+\z/
    halves = s.split("::", -1)
    return nil if halves.size > 2
    groups = halves.map { |h| h.empty? ? [] : h.split(":", -1) }
    tail = groups.last
    v4 = nil
    if !tail.empty? && tail.last.include?(".")
      v4 = ip_bytes(tail.pop)
      return nil if v4.nil? || v4.size != 4
    end
    words = []
    groups.each do |g|
      ws = []
      g.each do |w|
        return nil unless w =~ /\A[0-9A-Fa-f]{1,4}\z/
        ws << w.to_i(16)
      end
      words << ws
    end
    have = words.flatten.size + (v4.nil? ? 0 : 2)
    if halves.size == 2
      return nil if have > 7
      all = words[0] + Array.new(8 - have, 0) + (words[1] || [])
    else
      return nil if have != 8
      all = words[0]
    end
    bytes = []
    all.each { |w| bytes << (w >> 8) << (w & 0xff) }
    bytes.concat(v4) unless v4.nil?
    bytes
  end

  # Whether the network +net+ ("10.0.0.0/8", "fe80::/10", an address)
  # holds the address +addr+; nil when +net+ is no address.
  def self.ip_network_include?(net, addr)
    base, len = net.split("/", 2)
    nb = ip_bytes(base.to_s)
    return nil if nb.nil?
    bits = nb.size * 8
    prefix = bits
    unless len.nil?
      if len.include?(".")
        mask = ip_bytes(len)
        return nil if mask.nil? || mask.size != nb.size
        prefix = mask.map { |b| b.to_s(2).rjust(8, "0") }.join
        return nil unless prefix =~ /\A1*0*\z/
        prefix = prefix.count("1")
      else
        return nil unless len =~ /\A\d+\z/
        prefix = len.to_i
      end
    end
    return nil if prefix > bits
    ab = ip_bytes(addr.to_s)
    return false if ab.nil? || ab.size != nb.size
    full = prefix / 8
    i = 0
    while i < full
      return false if ab[i] != nb[i]
      i += 1
    end
    rest = prefix % 8
    return true if rest.zero?
    m = (0xff << (8 - rest)) & 0xff
    (ab[full] & m) == (nb[full] & m)
  end

  # The user and the password either side of a userinfo's first ":".
  def self.split_userinfo(ui)
    return [nil, nil] if ui.nil? || ui.empty?
    parts = ui.split(":", 2)
    [parts[0], parts[1]]
  end

  def self.join_userinfo(user, password)
    return nil if user.nil?
    password.nil? ? user : "#{user}:#{password}"
  end

  # A query as CRuby's #query= stores it: tabs and newlines dropped, and
  # every byte a query may not carry as "%XX"; a "%" before two non-hex
  # characters is refused.
  def self.normalize_query(v)
    return nil if v.nil?
    s = v.to_s.delete("\t\r\n")
    if (m = /%[^0-9A-Fa-f][^0-9A-Fa-f]/.match(s))
      raise InvalidURIError, "invalid percent escape: #{m[0]}"
    end
    out = String.new
    s.each_byte do |b|
      keep = b == 0x21 || (b >= 0x24 && b <= 0x26) || (b >= 0x28 && b <= 0x3b) || b == 0x3d ||
             (b >= 0x3f && b <= 0x5f) || (b >= 0x61 && b <= 0x7e)
      keep ? out << b.chr : out << format("%%%02X", b)
    end
    out.force_encoding(Encoding::US_ASCII)
  end

  # A fragment as CRuby's #fragment= stores it: tabs and newlines dropped,
  # every byte outside "!".."~" as "%XX".
  def self.normalize_fragment(v)
    return nil if v.nil?
    out = String.new
    v.to_s.delete("\t\r\n").each_byte do |b|
      b >= 0x21 && b <= 0x7e ? out << b.chr : out << format("%%%02X", b)
    end
    out.force_encoding(Encoding::US_ASCII)
  end

  VERSION = "1.1.2"
  VERSION_CODE = "010102"

  # The WHATWG Encoding Standard's labels, each to the encoding it names
  # (CRuby's table), which URI.get_encoding looks a label up in.
  WEB_ENCODINGS_ = {
    "unicode-1-1-utf-8" => "utf-8", "utf-8" => "utf-8", "utf8" => "utf-8", "866" => "ibm866",
    "cp866" => "ibm866", "csibm866" => "ibm866", "ibm866" => "ibm866", "csisolatin2" => "iso-8859-2",
    "iso-8859-2" => "iso-8859-2", "iso-ir-101" => "iso-8859-2", "iso8859-2" => "iso-8859-2", "iso88592" => "iso-8859-2",
    "iso_8859-2" => "iso-8859-2", "iso_8859-2:1987" => "iso-8859-2", "l2" => "iso-8859-2", "latin2" => "iso-8859-2",
    "csisolatin3" => "iso-8859-3", "iso-8859-3" => "iso-8859-3", "iso-ir-109" => "iso-8859-3", "iso8859-3" => "iso-8859-3",
    "iso88593" => "iso-8859-3", "iso_8859-3" => "iso-8859-3", "iso_8859-3:1988" => "iso-8859-3", "l3" => "iso-8859-3",
    "latin3" => "iso-8859-3", "csisolatin4" => "iso-8859-4", "iso-8859-4" => "iso-8859-4", "iso-ir-110" => "iso-8859-4",
    "iso8859-4" => "iso-8859-4", "iso88594" => "iso-8859-4", "iso_8859-4" => "iso-8859-4", "iso_8859-4:1988" => "iso-8859-4",
    "l4" => "iso-8859-4", "latin4" => "iso-8859-4", "csisolatincyrillic" => "iso-8859-5", "cyrillic" => "iso-8859-5",
    "iso-8859-5" => "iso-8859-5", "iso-ir-144" => "iso-8859-5", "iso8859-5" => "iso-8859-5", "iso88595" => "iso-8859-5",
    "iso_8859-5" => "iso-8859-5", "iso_8859-5:1988" => "iso-8859-5", "arabic" => "iso-8859-6", "asmo-708" => "iso-8859-6",
    "csiso88596e" => "iso-8859-6", "csiso88596i" => "iso-8859-6", "csisolatinarabic" => "iso-8859-6", "ecma-114" => "iso-8859-6",
    "iso-8859-6" => "iso-8859-6", "iso-8859-6-e" => "iso-8859-6", "iso-8859-6-i" => "iso-8859-6", "iso-ir-127" => "iso-8859-6",
    "iso8859-6" => "iso-8859-6", "iso88596" => "iso-8859-6", "iso_8859-6" => "iso-8859-6", "iso_8859-6:1987" => "iso-8859-6",
    "csisolatingreek" => "iso-8859-7", "ecma-118" => "iso-8859-7", "elot_928" => "iso-8859-7", "greek" => "iso-8859-7",
    "greek8" => "iso-8859-7", "iso-8859-7" => "iso-8859-7", "iso-ir-126" => "iso-8859-7", "iso8859-7" => "iso-8859-7",
    "iso88597" => "iso-8859-7", "iso_8859-7" => "iso-8859-7", "iso_8859-7:1987" => "iso-8859-7", "sun_eu_greek" => "iso-8859-7",
    "csiso88598e" => "iso-8859-8", "csisolatinhebrew" => "iso-8859-8", "hebrew" => "iso-8859-8", "iso-8859-8" => "iso-8859-8",
    "iso-8859-8-e" => "iso-8859-8", "iso-ir-138" => "iso-8859-8", "iso8859-8" => "iso-8859-8", "iso88598" => "iso-8859-8",
    "iso_8859-8" => "iso-8859-8", "iso_8859-8:1988" => "iso-8859-8", "visual" => "iso-8859-8", "csisolatin6" => "iso-8859-10",
    "iso-8859-10" => "iso-8859-10", "iso-ir-157" => "iso-8859-10", "iso8859-10" => "iso-8859-10", "iso885910" => "iso-8859-10",
    "l6" => "iso-8859-10", "latin6" => "iso-8859-10", "iso-8859-13" => "iso-8859-13", "iso8859-13" => "iso-8859-13",
    "iso885913" => "iso-8859-13", "iso-8859-14" => "iso-8859-14", "iso8859-14" => "iso-8859-14", "iso885914" => "iso-8859-14",
    "csisolatin9" => "iso-8859-15", "iso-8859-15" => "iso-8859-15", "iso8859-15" => "iso-8859-15", "iso885915" => "iso-8859-15",
    "iso_8859-15" => "iso-8859-15", "l9" => "iso-8859-15", "iso-8859-16" => "iso-8859-16", "cskoi8r" => "koi8-r",
    "koi" => "koi8-r", "koi8" => "koi8-r", "koi8-r" => "koi8-r", "koi8_r" => "koi8-r",
    "koi8-ru" => "koi8-u", "koi8-u" => "koi8-u", "dos-874" => "windows-874", "iso-8859-11" => "windows-874",
    "iso8859-11" => "windows-874", "iso885911" => "windows-874", "tis-620" => "windows-874", "windows-874" => "windows-874",
    "cp1250" => "windows-1250", "windows-1250" => "windows-1250", "x-cp1250" => "windows-1250", "cp1251" => "windows-1251",
    "windows-1251" => "windows-1251", "x-cp1251" => "windows-1251", "ansi_x3.4-1968" => "windows-1252", "ascii" => "windows-1252",
    "cp1252" => "windows-1252", "cp819" => "windows-1252", "csisolatin1" => "windows-1252", "ibm819" => "windows-1252",
    "iso-8859-1" => "windows-1252", "iso-ir-100" => "windows-1252", "iso8859-1" => "windows-1252", "iso88591" => "windows-1252",
    "iso_8859-1" => "windows-1252", "iso_8859-1:1987" => "windows-1252", "l1" => "windows-1252", "latin1" => "windows-1252",
    "us-ascii" => "windows-1252", "windows-1252" => "windows-1252", "x-cp1252" => "windows-1252", "cp1253" => "windows-1253",
    "windows-1253" => "windows-1253", "x-cp1253" => "windows-1253", "cp1254" => "windows-1254", "csisolatin5" => "windows-1254",
    "iso-8859-9" => "windows-1254", "iso-ir-148" => "windows-1254", "iso8859-9" => "windows-1254", "iso88599" => "windows-1254",
    "iso_8859-9" => "windows-1254", "iso_8859-9:1989" => "windows-1254", "l5" => "windows-1254", "latin5" => "windows-1254",
    "windows-1254" => "windows-1254", "x-cp1254" => "windows-1254", "cp1255" => "windows-1255", "windows-1255" => "windows-1255",
    "x-cp1255" => "windows-1255", "cp1256" => "windows-1256", "windows-1256" => "windows-1256", "x-cp1256" => "windows-1256",
    "cp1257" => "windows-1257", "windows-1257" => "windows-1257", "x-cp1257" => "windows-1257", "cp1258" => "windows-1258",
    "windows-1258" => "windows-1258", "x-cp1258" => "windows-1258", "x-mac-cyrillic" => "macCyrillic", "x-mac-ukrainian" => "macCyrillic",
    "chinese" => "gbk", "csgb2312" => "gbk", "csiso58gb231280" => "gbk", "gb2312" => "gbk",
    "gb_2312" => "gbk", "gb_2312-80" => "gbk", "gbk" => "gbk", "iso-ir-58" => "gbk",
    "x-gbk" => "gbk", "gb18030" => "gb18030", "big5" => "big5", "big5-hkscs" => "big5",
    "cn-big5" => "big5", "csbig5" => "big5", "x-x-big5" => "big5", "cseucpkdfmtjapanese" => "cp51932",
    "euc-jp" => "cp51932", "x-euc-jp" => "cp51932", "csiso2022jp" => "cp50221", "iso-2022-jp" => "cp50221",
    "csshiftjis" => "Windows-31J", "ms932" => "Windows-31J", "ms_kanji" => "Windows-31J", "shift-jis" => "Windows-31J",
    "shift_jis" => "Windows-31J", "sjis" => "Windows-31J", "windows-31j" => "Windows-31J", "x-sjis" => "Windows-31J",
    "cseuckr" => "euc-kr", "csksc56011987" => "euc-kr", "euc-kr" => "euc-kr", "iso-ir-149" => "euc-kr",
    "korean" => "euc-kr", "ks_c_5601-1987" => "euc-kr", "ks_c_5601-1989" => "euc-kr", "ksc5601" => "euc-kr",
    "ksc_5601" => "euc-kr", "windows-949" => "euc-kr", "utf-16be" => "utf-16be", "utf-16" => "utf-16le",
    "utf-16le" => "utf-16le",
  }

  # The Encoding a web label names ("latin1" is Windows-1252, as the
  # Encoding Standard has it), nil for one it does not.
  def self.get_encoding(label)
    name = WEB_ENCODINGS_[label.to_str.strip.downcase]
    return nil if name.nil?
    begin
      Encoding.find(name)
    rescue ArgumentError
      nil
    end
  end

  # CRuby sets these from the default parser's #regexp table when it loads
  # (URI.parser=, which cannot rebind a constant here, is absent).
  SCHEME = RFC3986_PARSER.regexp[:SCHEME]
  USERINFO = RFC3986_PARSER.regexp[:USERINFO]
  HOST = RFC3986_PARSER.regexp[:HOST]
  ABS_PATH = RFC3986_PARSER.regexp[:ABS_PATH]
  REL_PATH = RFC3986_PARSER.regexp[:REL_PATH]
  QUERY = RFC3986_PARSER.regexp[:QUERY]
  FRAGMENT = RFC3986_PARSER.regexp[:FRAGMENT]
  OPAQUE = RFC3986_PARSER.regexp[:OPAQUE]
  PORT = RFC3986_PARSER.regexp[:PORT]

  # The byte tables CRuby's codecs read: each byte to its "%XX" (a space
  # as "+" for www-form), and each "%XX" in any letter case to its byte.
  TBLENCURICOMP_ = (0..255).map { |i| [i.chr, format("%%%02X", i)] }.to_h
  TBLENCWWWCOMP_ = TBLENCURICOMP_.merge({ " " => "+" })
  TBLDECWWWCOMP_ = (0..255).flat_map do |i|
    h = i >> 4
    l = i & 15
    [format("%%%X%X", h, l), format("%%%x%X", h, l), format("%%%X%x", h, l), format("%%%x%x", h, l)].map { |k| [k, i.chr] }
  end.to_h.merge({ "+" => " " })

  module Util
    # A scheme class's components from an Array (after the scheme) or a
    # Hash, with the scheme from the class name.
    def self.make_components_hash(klass, array_hash)
      klass.components_hash(array_hash, {})
    end
  end

  def self.split(uri)
    RFC3986_PARSER.split(uri)
  end

  def self.parse(uri)
    RFC3986_PARSER.parse(uri)
  end

  # `URI.join("http://example.com/a/", "b", "../c")`: the first argument
  # parsed, then each of the rest merged onto it in turn.
  def self.join(*str)
    RFC3986_PARSER.join(*str)
  end

  # Every absolute URI in +str+ (of one of +schemes+, when given); CRuby
  # marks it obsolete.
  def self.extract(str, schemes = nil)
    warn "URI.extract is obsolete" if $VERBOSE
    if block_given?
      RFC2396_PARSER.extract(str, schemes) { |u| yield u }
    else
      RFC2396_PARSER.extract(str, schemes)
    end
  end

  def self.regexp(schemes = nil)
    warn "URI.regexp is obsolete" if $VERBOSE
    RFC2396_PARSER.make_regexp(schemes)
  end

  # #merge's argument: a URI as it is, a String parsed.
  def self.for_merge(oth)
    if oth.is_a?(Generic)
      oth
    elsif oth.is_a?(String)
      parse(oth)
    else
      raise ArgumentError, "bad argument (expected URI object or URI string)"
    end
  end
end

# `URI("https://example.com/x")` -- the Kernel method every caller writes.
def URI(str)
  str.is_a?(URI::Generic) ? str : URI.parse(str)
end

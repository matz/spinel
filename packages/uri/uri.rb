# Spinel bundled `uri` -- the generic/HTTP/HTTPS half.
#
# The spelling is CRuby's. What is here is what an HTTP client reaches:
# `URI(str)`, the component readers, #request_uri and #to_s. What is not here
# is absent the way a subset is absent things, and a program naming it fails
# to compile rather than at run time:
#
# * only http, https and a bare/relative form parse; there is no URI::FTP,
#   URI::LDAP, URI::MailTo, URI::WS, or the scheme registry behind them
# * userinfo, fragment and opaque are parsed and readable, but there is no
#   #merge, #route_to, #normalize or #select
# * URI.encode_www_form_component / .decode_www_form_component are here
#   because a query builder needs them; the URI.escape family CRuby removed
#   is not
module URI
  class Error < StandardError
  end

  class InvalidURIError < Error
  end

  class Generic
    attr_reader :scheme, :userinfo, :host, :port, :path, :query, :fragment
    # The component writers CRuby's Generic has: a parsed URI is edited
    # in place and re-serialised with #to_s (`uri.host = "fxtwitter.com"`
    # is how a Rails app swaps a domain). Assignment only -- CRuby also
    # validates the new component and raises InvalidComponentError on a
    # bad one, which is absent here the way the rest of the subset is.
    attr_writer :scheme, :userinfo, :host, :port, :path, :query, :fragment

    def hostname=(value)
      @host = value
    end

    def initialize(scheme, userinfo, host, port, path, query, fragment)
      @scheme = scheme
      @userinfo = userinfo
      @host = host
      @port = port
      @path = path
      @query = query
      @fragment = fragment
    end

    def default_port
      case @scheme
      when "https" then 443
      when "http" then 80
      else 0
      end
    end

    # What goes on the request line: the path with the query, and "/" when the
    # path is empty -- `URI("http://example.com").request_uri` is "/".
    def request_uri
      p = (@path.nil? || @path.empty?) ? "/" : @path
      @query.nil? || @query.empty? ? p : "#{p}?#{@query}"
    end

    # the host without the brackets an IPv6 address carries in a URI, as
    # CRuby's URI::Generic#hostname answers it
    def hostname
      v = @host
      v && v.start_with?("[") && v.end_with?("]") ? v[1..-2] : v
    end

    def to_s
      s = String.new
      s << "#{@scheme}://" unless @scheme.nil? || @scheme.empty?
      s << "#{@userinfo}@" unless @userinfo.nil? || @userinfo.empty?
      s << @host.to_s
      # A cleared port (`uri.port = nil`) is omitted, as CRuby omits it.
      s << ":#{@port}" if !@port.nil? && @port != default_port && @port > 0
      s << @path.to_s
      s << "?#{@query}" unless @query.nil? || @query.empty?
      s << "##{@fragment}" unless @fragment.nil? || @fragment.empty?
      s
    end

    def inspect
      "#<#{self.class}: #{self}>"
    end

    def ==(other)
      other.is_a?(Generic) && to_s == other.to_s
    end
  end

  # `URI::HTTPS.build(host: "api.github.com", path: "/x")` -- a URI from
  # its components rather than from a string. CRuby also takes an Array
  # of components in order, and validates each one; the keyword form is
  # the one an application writes, and validation is absent here the way
  # it is for the component writers above. The port is left nil unless
  # given, so #to_s leaves it off exactly as CRuby does for the default.
  class HTTP < Generic
    def self.build(userinfo: nil, host: nil, port: nil, path: "", query: nil, fragment: nil)
      HTTP.new("http", userinfo, host, port, path, query, fragment)
    end
  end

  class HTTPS < HTTP
    def self.build(userinfo: nil, host: nil, port: nil, path: "", query: nil, fragment: nil)
      HTTPS.new("https", userinfo, host, port, path, query, fragment)
    end
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

  def self.decode_www_form_component(str)
    out = String.new
    s = str.to_s
    i = 0
    while i < s.length
      ch = s[i]
      if ch == "+"
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
    out
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
          values << (item.nil? ? key : "#{key}=#{encode_www_form_component(item)}")
        end
        parts << values.join("&")
      else
        parts << "#{key}=#{encode_www_form_component(v)}"
      end
    end
    parts.join("&")
  end

  # The characters RFC 3986 excludes from a URI: the space and the
  # control range, plus the "unwise" set a generic URI may not carry
  # unescaped, and the brackets an authority reserves for an IPv6
  # literal. CRuby's parser rejects a string containing any of them
  # with `InvalidURIError`, and an app can be RELYING on that rescue
  # rather than on its own validation -- a Rails message body decides
  # whether to keep a link by asking whether `URI.parse` accepted it,
  # so a parser that accepts everything turns that check into a no-op.
  # `"http://exa mple.com/ "` is the shape that surfaced it: with a
  # space admitted, the host reads as an ordinary off-site domain.
  INVALID_URI_CHARS = " <>\"{}|\\^`[]"

  # Where each of them is excluded is not uniform, and CRuby's own parser is
  # what this follows: the QUERY (between the first `?` and the first `#`)
  # takes any ASCII character, including a space and the unwise set, so
  # `?filter=a|b` parses; everywhere else -- the scheme, the authority, the
  # path and the fragment -- the set above is rejected. A bracket is the
  # exception inside an authority, where `http://[::1]/x` is an IPv6 host.
  # A byte outside ASCII is rejected everywhere, query included.
  def self.invalid_char?(s)
    qi = s.index("?")
    fi = s.index("#")
    qi = nil if qi && fi && fi < qi          # `#a?b` is all fragment
    auth_end = -1
    ai = s.index("://")
    if ai
      auth_end = s.length
      i = ai + 3
      while i < s.length
        c = s[i]
        if c == "/" || c == "?" || c == "#"
          auth_end = i
          break
        end
        i += 1
      end
    end
    # the host inside the authority, after any `user:pw@`
    host_start = -1
    host_end = -1
    if ai
      host_start = ai + 3
      j = host_start
      while j < auth_end
        host_start = j + 1 if s[j] == "@"
        j += 1
      end
      host_end = auth_end
      j = host_start
      while j < auth_end
        if s[j] == "]"
          host_end = j + 1
          break
        end
        j += 1
      end
    end
    i = 0
    while i < s.length
      c = s[i]
      o = c.ord
      return true if o > 0x7f
      in_query = qi && i > qi && (fi.nil? || i < fi)
      unless in_query
        return true if o < 0x20 || o == 0x7f
        if INVALID_URI_CHARS.include?(c)
          # a bracketed IPv6 host: `[` opens the host and `]` closes it, with
          # only a `:port` allowed after. Anything else bracketed is rejected
          # the way CRuby rejects `http://exa[mple.com/`.
          bracket_host = (c == "[" && i == host_start) ||
                         (c == "]" && i == host_end - 1 && host_end > host_start + 1 &&
                          s[host_start] == "[")
          return true unless bracket_host
        end
      end
      i += 1
    end
    false
  end

  def self.parse(str)
    s = str.to_s
    raise InvalidURIError, "bad URI (is not URI?): #{s.inspect}" if invalid_char?(s)
    scheme = ""
    rest = s
    idx = s.index("://")
    if idx
      scheme = s[0, idx].downcase
      rest = s[(idx + 3)..-1].to_s
    end

    fragment = ""
    fi = rest.index("#")
    if fi
      fragment = rest[(fi + 1)..-1].to_s
      rest = rest[0, fi]
    end

    query = ""
    qi = rest.index("?")
    if qi
      query = rest[(qi + 1)..-1].to_s
      rest = rest[0, qi]
    end

    authority = rest
    path = ""
    pi = rest.index("/")
    if pi
      authority = rest[0, pi]
      path = rest[pi..-1].to_s
    end

    userinfo = ""
    ai = authority.index("@")
    if ai
      userinfo = authority[0, ai]
      authority = authority[(ai + 1)..-1].to_s
    end

    host = authority
    port = 0
    ci = authority.rindex(":")
    if ci && !authority[(ci + 1)..-1].to_s.empty? &&
       authority[(ci + 1)..-1].to_s =~ /\A[0-9]+\z/
      host = authority[0, ci]
      port = authority[(ci + 1)..-1].to_i
    end

    if port == 0
      port = 443 if scheme == "https"
      port = 80 if scheme == "http"
    end

    if scheme == "https"
      HTTPS.new(scheme, userinfo, host, port, path, query, fragment)
    elsif scheme == "http"
      HTTP.new(scheme, userinfo, host, port, path, query, fragment)
    else
      Generic.new(scheme, userinfo, host, port, path, query, fragment)
    end
  end

  # The RFC 2396 parser's #make_regexp, the pattern CRuby builds for an
  # absolute URI. The rest of the parser (#split, #parse, #escape, the
  # pattern and regexp tables) is not here.
  class RFC2396_Parser
    def make_regexp(schemes = nil)
      x = x_abs_uri
      return Regexp.new(x, Regexp::EXTENDED) unless schemes
      Regexp.new("(?=(?i:#{Regexp.union(*schemes).source}):)#{x}", Regexp::EXTENDED)
    end

    private

    # The pieces of CRuby's initialize_pattern the absolute-URI pattern uses.
    def x_abs_uri
      alpha = "a-zA-Z"
      alnum = "#{alpha}\\d"
      hex = "a-fA-F\\d"
      escaped = "%[#{hex}]{2}"
      unreserved = "\\-_.!~*'()#{alnum}"
      reserved = ";/?:@&=+$,\\[\\]"
      uric = "(?:[#{unreserved}#{reserved}]|#{escaped})"
      uric_no_slash = "(?:[#{unreserved};?:@&=+$,]|#{escaped})"
      query = "#{uric}*"
      fragment = "#{uric}*"
      hostname = "(?:[a-zA-Z0-9\\-.]|%\\h\\h)+"
      ipv4addr = "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}"
      hex4 = "[#{hex}]{1,4}"
      lastpart = "(?:#{hex4}|#{ipv4addr})"
      hexseq1 = "(?:#{hex4}:)*#{hex4}"
      hexseq2 = "(?:#{hex4}:)*#{lastpart}"
      ipv6addr = "(?:#{hexseq2}|(?:#{hexseq1})?::(?:#{hexseq2})?)"
      ipv6ref = "\\[#{ipv6addr}\\]"
      host = "(?:#{hostname}|#{ipv4addr}|#{ipv6ref})"
      userinfo = "(?:[#{unreserved};:&=+$,]|#{escaped})*"
      pchar = "(?:[#{unreserved}:@&=+$,]|#{escaped})"
      param = "#{pchar}*"
      segment = "#{pchar}*(?:;#{param})*"
      path_segments = "#{segment}(?:/#{segment})*"
      reg_name = "(?:[#{unreserved}$,;:@&=+]|#{escaped})+"
      scheme = "[#{alpha}][\\-+.#{alpha}\\d]*"
      abs_path = "/#{path_segments}"
      opaque_part = "#{uric_no_slash}#{uric}*"
      "
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
    end
  end

  RFC2396_PARSER = RFC2396_Parser.new

  def self.normalize_path(path)
    trailing_slash = path.end_with?("/") || path.end_with?("/.") || path.end_with?("/..")
    parts = []
    path.split("/").each do |part|
      if part == ".."
        parts.pop if parts.length > 1
      elsif part != "."
        parts << part
      end
    end
    normalized = parts.join("/")
    normalized << "/" if trailing_slash && !normalized.end_with?("/")
    normalized
  end

  def self.join(base, rel)
    b = parse(base.to_s)
    r = rel.to_s
    return parse(r) if r.include?("://")
    q = ""
    qi = r.index("?")
    if qi
      q = r[(qi + 1)..-1].to_s
      r = r[0, qi]
    end
    if r.start_with?("/")
      path = r
    else
      dir = b.path.to_s
      cut = dir.rindex("/")
      dir = cut ? dir[0, cut + 1] : "/"
      path = dir + r
    end
    path = normalize_path(path)
    if b.scheme == "https"
      HTTPS.new(b.scheme, b.userinfo, b.host, b.port, path, q, "")
    else
      HTTP.new(b.scheme, b.userinfo, b.host, b.port, path, q, "")
    end
  end
end

# `URI("https://example.com/x")` -- the Kernel method every caller writes.
def URI(str)
  str.is_a?(URI::Generic) ? str : URI.parse(str)
end

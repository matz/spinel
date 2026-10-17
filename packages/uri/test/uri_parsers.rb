# URI's parsers and module functions, diffed against CRuby: URI.split on
# RFC 3986's grammar (IPv6, IPvFuture and IPv4 hosts, opaque and relative
# references, what it refuses), the RFC 2396 parser's split / parse /
# escape / unescape / extract / pattern table, the scheme registry and
# URI.for, URI.get_encoding, and the constants.
require "uri"

def show
  p yield
rescue URI::Error, ArgumentError => e
  p [e.class, e.message]
end

[
  "http://user:pw@[::1]:8080/a/b?q=1#f", "https://[v1.fe]/x", "http://192.168.0.1/",
  "http://a/?q=a b", "urn:isbn:0451450523", "mailto:a@b.com?subject=x", "//host/path",
  "../rel/path?x", "", "#frag", "http://a:/p", "HTTP://Example.COM/X", "http://a/b c",
  "http://a/\u00e9", "foo:/abs", "http://[::1/", "http://a/%zz",
].each { |u| show { URI.split(u) } }
show { URI.parse("HTTP://Example.COM:/x").to_s }
show { URI.parse("http://a/b c") }
show { URI.parse("mailto://x") }

r = URI::RFC2396_PARSER
show { r.split("http://u@h:1/p;x?q#f") }
show { r.split("foo://reg;name/x") }
show { r.parse("foo://reg;name/x") }
show { r.parse("http://a b/") }
show { r.split("rel/seg/path?q") }
show { r.parse("http://example.com/a").class }
show { r.escape("a b/c?d=é") }
show { r.escape("a@b:c", "@:") }
show { r.escape("x/y", /[\/]/) }
show { r.unescape("a%20b%2Fc%E3%81%82") }
show { r.join("http://a/b/", "c", "../d").to_s }
show { r.extract("see http://a.com/x and mailto:b@c.com, ftp://f/ too") }
show { r.extract("http://a.com https://b.com", ["https"]) }
found = []
show { r.extract("x http://a.com y http://b.org") { |u| found << u } }
p found
show { r.make_regexp(["http"]).match?("HTTP://x") }
show { r.pattern[:HOST] }
show { r.pattern.keys }
show { r.regexp.keys }
show { URI::RFC2396_Parser.new(HOSTNAME: "[a-z]+").pattern[:HOST] }
show { URI::DEFAULT_PARSER.escape("a b") }
show { URI::DEFAULT_PARSER.unescape("a%20b") }
show { URI::RFC3986_PARSER.regexp.keys }
show { URI::RFC3986_PARSER.regexp[:HOST].match?("[::1]") }
show { URI.extract("go to http://a.com/x now") }
show { URI.regexp(["ftp"]).match?("ftp://x/") }
show { URI.join("http://a/b/c", "../d", "e?x").to_s }

%i[ALPHA ALNUM HEX ESCAPED UNRESERVED RESERVED DOMLABEL TOPLABEL HOSTNAME URIC URIC_NO_SLASH QUERY FRAGMENT
   IPV4ADDR IPV6ADDR IPV6REF HOST PORT HOSTPORT USERINFO PATH_SEGMENTS REG_NAME REL_SEGMENT SCHEME ABS_PATH
   REL_PATH NET_PATH HIER_PART OPAQUE_PART ABS_URI REL_URI URI_REF].each do |c|
  p [c, URI::RFC2396_REGEXP::PATTERN.const_get(c) == URI::RFC2396_Parser.new.pattern.fetch(c, URI::RFC2396_REGEXP::PATTERN.const_get(c))]
end

class GopherURI < URI::Generic
  def self.default_port
    70
  end
end
URI.register_scheme("GOPHER", GopherURI)
g = URI("gopher://example.org/1/x")
p [g.class, g.port, g.to_s]
# CRuby lists them in its constant table's order, which a registration reshuffles.
p URI.scheme_list.keys.sort
p URI.scheme_list["GOPHER"]
p URI.for("http", nil, "h", nil, nil, "/p", nil, nil, nil).to_s
p URI.for("unknown", nil, "h", nil, nil, "/p", nil, nil, nil).class
p URI.for("unknown", nil, "h", nil, nil, "/p", nil, nil, nil, default: URI::HTTP).class

%w[utf8 latin1 shift_jis euc-jp UTF-16LE  sjis bogus].each { |l| p URI.get_encoding(l) }
p [URI::VERSION, URI::VERSION_CODE, URI::Parser, URI::PARSER.class, URI::DEFAULT_PARSER.class, URI::REGEXP]

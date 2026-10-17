# URI's component writers and build, diffed against CRuby: each checks
# its component against the grammar of the parser that made the URI and
# raises InvalidComponentError for a bad one (InvalidURIError for one the
# URI's other parts rule out); #query= and #fragment= escape what they
# may not carry; #host= and #port= drop the userinfo, as CRuby's do.
require "uri"

def t
  p yield
rescue URI::Error => e
  p [e.class, e.message]
end

t { u = URI("http://a:b@h/x"); u.host = "y"; u.to_s }
t { u = URI("http://a:b@h/x"); u.port = 81; u.to_s }
t { u = URI("http://a:b@h/x"); u.userinfo = nil; u.to_s }
t { u = URI("http://h/x"); u.userinfo = ["me", "pw"]; u.to_s }
t { u = URI("http://h/x"); u.userinfo = "me:p:w"; [u.user, u.password] }
t { u = URI("http://u:p@h/x"); u.password = nil; u.to_s }
t { URI("http://h/x").password = "pw" }
t { URI("http://h/x").userinfo = "a b" }
t { URI("http://u@h/x").password = "a b" }
t { u = URI("http://h/x"); u.query = "a b\t\"c\"<d>%zz"; u.query }
t { u = URI("http://h/x"); u.query = "a b\t\"c\"<d>%2z é"; u.query }
t { u = URI("http://h/x"); u.fragment = "a b é#x"; u.fragment }
t { u = URI("http://h/x"); u.query = nil; u.to_s }
t { URI("http://h/x?a b").query }
t { URI("http://h/x#a b").fragment }
t { URI("http://h/x?%zz") }
t { u = URI("http://h/x"); u.hostname = "::1"; u.to_s }
t { u = URI("http://h/x"); u.port = "8080"; u.port }
t { u = URI("http://h/x"); u.port = ""; u.port }
t { URI("http://h/x").port = "x1" }
t { u = URI("http://h/x"); u.scheme = "HTTPS"; u.to_s }
t { URI("http://h/x").scheme = "1bad" }
t { URI("http://h/x").host = "a b" }
t { URI("http://h/x").path = "rel" }
t { u = URI("http://h/x"); u.path = "/ok/%41"; u.to_s }
t { u = URI("ftp://h/x"); u.path = "rel/ok"; u.to_s }
t { URI("http://h/x").opaque = "x" }
t { URI("mailto:a@b.c").host = "x" }
t { URI("mailto:a@b.c").port = 1 }
t { URI("mailto:a@b.c").path = "/x" }
t { URI("mailto:a@b.c").query = "x" }
t { URI("mailto:a@b.c").user = "u" }
t { URI("http://h/x").registry = "x" }
t { u = URI::Generic.build(scheme: "urn"); u.opaque = "isbn:1"; u.to_s }
t { URI::HTTP.build(host: "a b") }
t { URI::HTTP.build(host: "::1", port: "8080").to_s }
t { URI::Generic.build(path: "rel", host: "h").to_s }
t { URI::Generic.build(path: "rel").to_s }
t { URI::Generic.build(userinfo: "u:p", host: "h").to_s }
t { URI::File.build(userinfo: "u", path: "/x") }
t { URI::File.build(host: "localhost", path: "/x").to_s }
t { u = URI("file:///x"); u.host = "localhost"; u.host }
t { URI::LDAP.build(host: "h", dn: "dc=x") }
t { URI::MailTo.build(to: "not an address") }
t { URI::MailTo.build(to: "a@b.com", headers: "a b") }
t { URI::HTTP.build(query: "a b", fragment: "c d").to_s }

r = URI::RFC2396_PARSER.parse("http://h/x")
t { r.host = "a_b"; r.to_s }
t { URI("http://h/x").host = "a_b" }
t { r.path = "/a b" }
t { URI::HTTP.build(host: "").to_s }
t { URI::HTTPS.build(host: "").to_s }
t { URI::WS.build(host: "").to_s }
t { URI::Generic.build(host: "").to_s }

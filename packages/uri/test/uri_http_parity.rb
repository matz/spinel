# The smaller corners of CRuby's uri, diffed against it: HTTP refuses an
# empty host, #authority and #request_uri answer as CRuby's do (a nil
# port spelled out, no path answering nil), the byte tables and
# URI::Util, the regexp constants the default parser sets, and File's
# refusing checks.
require "uri"

def t
  p yield
rescue URI::Error, NoMethodError => e
  p [e.class, e.message]
end

t { URI("http://h/x").host = "" }
t { u = URI("ws://h/x"); u.host = ""; u.to_s }
t { u = URI("http://h:80/x"); u.port = nil; u.authority }
t { URI("http://h:8080/x").authority }
t { URI("http://h/x").origin }
t { URI("http://h").request_uri }
t { URI("urn:isbn:1").request_uri }
t { URI("http://h/a?b=1").request_uri }
t { URI("ws://h/a").request_uri }

t { [URI::TBLENCWWWCOMP_[" "], URI::TBLENCWWWCOMP_["/"], URI::TBLENCWWWCOMP_.size] }
t { [URI::TBLENCURICOMP_[" "], URI::TBLDECWWWCOMP_["%2f"], URI::TBLDECWWWCOMP_["%2F"], URI::TBLDECWWWCOMP_["+"]] }
t { URI::Util.make_components_hash(URI::HTTP, { host: "h" }) }
t { URI::Util.make_components_hash(URI::FTP, ["u", "h", 21, "p", "i"]) }
t { [URI::SCHEME, URI::ABS_PATH, URI::PORT].map(&:class) }
t { URI::HOST.match?("[::1]") }
t { URI::ABS_PATH.match?("/a/b") }
t { URI::REL_PATH.match?("/a") }

f = URI("file:///etc/x")
t { f.user = "a" }
t { f.password = "a" }
t { f.userinfo = nil; f.to_s }

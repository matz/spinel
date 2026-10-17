# The scheme classes' own API, diffed against CRuby: build from keywords,
# a Hash or an Array of the components after the scheme; FTP's typecode;
# File's host and refused userinfo; MailTo's to/headers writers and
# RFC 822 text; LDAP's dn, attributes, scope, filter and extensions.
require "uri"

def err
  yield
rescue URI::Error, ArgumentError => e
  p [e.class, e.message]
end

p URI::HTTP.build(["u:p", "example.com", 8080, "/a", "q=1", "f"]).to_s
p URI::HTTP.build({ host: "example.com", path: "/x" }).to_s
p URI::HTTPS.build(host: "example.com", path: "/s").to_s
p URI::Generic.build(["git", nil, "h", nil, nil, "/r", nil, nil, nil]).to_s
p URI::Generic.build(scheme: "urn", opaque: "isbn:1").to_s
err { URI::HTTP.build(["too", "few"]) }

f = URI::FTP.build(host: "ftp.example.com", path: "pub/file.txt", typecode: "i")
p [f.to_s, f.path, f.typecode]
p URI::FTP.build(["u", "ftp.example.com", nil, "/etc/x", nil]).to_s
p URI("ftp://ftp.example.com/a;type=d").typecode
f.typecode = "a"
p f.to_s
err { f.typecode = "z" }
g = URI("ftp://h/dir/file;type=i").merge(URI("ftp://h/dir/other;type=a"))
p [g.to_s, g.typecode]
p URI::FTP.component

h = URI("file://localhost/etc/hosts")
p [h.host, h.port, h.userinfo, h.to_s]
p URI::File.build(path: "/tmp/x").to_s
h.host = "localhost"
p h.host
err { h.userinfo = "a" }
err { h.user = "a" }
h.port = 99
p h.port

p URI::WS.build(host: "example.com", path: "/cable").to_s
p URI::WSS.build(["", "example.com", 8443, "/c", "x=1"]).to_s
p URI("ws://example.com").request_uri
p URI("http://@example.com/").to_s

m = URI::MailTo.build(to: ["a@example.com", "b@example.com"], headers: [["subject", "Hi"], ["body", "Yo"]])
p [m.to_s, m.to, m.headers]
p URI::MailTo.build(["c@example.com", { "cc" => "d@example.com" }]).to_s
p URI::MailTo.build(to: "e@example.com").to_s
m.to = "z@example.com"
m.headers = "subject=New&body=Text%20here"
p [m.to_s, m.to, m.headers]
p m.to_mailtext
p URI("mailto:a@b.com?subject=Hi%20there&to=x@y.com&cc=c@d.com").to_rfc822text
err { m.to = "not an address" }
err { m.headers = "a b" }
err { URI("mailto:@@") }
p URI::MailTo.component

l = URI("ldap://ldap.example.com/dc=example,dc=com?cn,mail?sub?(uid=a)")
p [l.class, l.dn, l.attributes, l.scope, l.filter, l.extensions, l.port, l.hierarchical?]
l.scope = "one"
l.filter = "(cn=*)"
p l.to_s
l.dn = "ou=people"
p l.to_s
p URI::LDAP.build(host: "h", dn: "/dc=x", attributes: "cn", scope: "base").to_s
p URI::LDAP.build(["h", 3389, "/dc=y", nil, nil, nil, nil]).to_s
p URI("ldaps://secure.example.com/dc=s").port
p URI("ldap://h/dc=z").select(:dn, :attributes)
p URI::LDAP::SCOPE
p URI::LDAP.component
err { URI("ldap://h/dc=x#frag") }

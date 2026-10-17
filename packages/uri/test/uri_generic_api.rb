# URI::Generic's reference, comparison and component API, diffed against
# CRuby: #route_from / #route_to / #- (the inverse of #merge), #merge!,
# #normalize!, #select, #component, #==/#eql?/#hash after normalizing,
# #hierarchical?, #coerce, #user= / #password= and the decoded readers.
require "uri"

base = URI("http://example.com/a/b/c")
p URI("http://example.com/a/b/d").route_from(base).to_s
p (URI("http://example.com/a/x/y") - base).to_s
p base.route_to("http://example.com/a/b/c?q=1").to_s
p base.route_to("http://example.com/a/").to_s
p base.route_to("http://example.com/z").to_s
p base.route_to("http://other.com/a").to_s
p base.route_to("https://example.com/a").to_s
p base.route_to("http://example.com/a/b/c").to_s
p base.route_to("http://example.com/a/b/c#frag").to_s
p URI("http://example.com:8080/a/b").route_to("http://example.com:8080/a/c").to_s
begin
  URI("/rel").route_to("http://example.com/")
rescue URI::BadURIError => e
  p e.message
end

m = URI("http://example.com/a/b")
p m.merge!("c").to_s
p m.to_s
p m.merge!("http://example.com/a/c").nil?

n = URI("HTTP://EXAMPLE.com")
p n.normalize!
p n.to_s
p URI("http://Example.COM").normalize.to_s

s = URI("http://user:pw@example.com:8080/p?q#f")
p s.select(:host, :port, :path)
p s.component
p URI::HTTP.component
p URI::Generic.component
begin
  s.select(:nope)
rescue ArgumentError => e
  p e.message
end

p URI("http://A.com/") == URI("http://a.com/")
p URI("http://a.com/") == URI("https://a.com/")
p URI("http://a.com/").eql?(URI("http://a.com/"))
p URI("http://A.com/").eql?(URI("http://a.com/"))
p URI("http://a.com/x").hash == URI("http://a.com/x").hash
p({ URI("http://a.com/x") => 1 }[URI("http://a.com/x")])

p URI("http://a.com/x").hierarchical?
p URI("mailto:a@b.com").hierarchical?
p URI("http://a.com").absolute
p URI("http://a.com/x").to_str
a, b = URI("http://a.com/").coerce("http://b.com/x")
p [a.class, a.to_s, b.to_s]

u = URI("http://alice%40x:p%3Aw@example.com/")
p [u.user, u.password, u.decoded_user, u.decoded_password]
u.password = "new"
p u.to_s
u.user = "bob"
p [u.user, u.password, u.to_s]
u.user = "carol:secret"
p [u.user, u.password, u.to_s]

# URI::Generic#find_proxy against an environment Hash, diffed against
# CRuby: the scheme's lower-case variable first, the CGI rule for
# HTTP_PROXY, no proxy for a loopback host, and no_proxy matched by host,
# by domain, by port and by network. Hosts are IP literals, so nothing is
# resolved over the network.
require "uri"

def show(uri, env)
  r = URI(uri).find_proxy(env)
  p r.nil? ? nil : r.to_s
end

show("http://10.1.2.3/", { "http_proxy" => "http://proxy:8080" })
show("http://10.1.2.3/", { "https_proxy" => "http://proxy:8080" })
show("https://10.1.2.3/", { "https_proxy" => "http://sproxy:3128" })
show("https://10.1.2.3/", { "HTTPS_PROXY" => "http://sproxy:3128" })
show("ftp://10.1.2.3/", { "FTP_PROXY" => "http://fproxy:21" })
show("http://10.1.2.3/", { "http_proxy" => "" })
show("http://127.0.0.1/", { "http_proxy" => "http://proxy:8080" })
show("http://10.1.2.3/", { "http_proxy" => "http://proxy:8080", "no_proxy" => "10.1.2.3" })
show("http://10.1.2.3:81/", { "http_proxy" => "http://proxy:8080", "no_proxy" => "10.1.2.3:80" })
show("http://10.1.2.3/", { "http_proxy" => "http://proxy:8080", "NO_PROXY" => "10.0.0.0/8" })
show("http://10.1.2.3/", { "http_proxy" => "http://proxy:8080", "no_proxy" => "192.168.0.0/16, other" })
show("http://10.1.2.3/", { "REQUEST_METHOD" => "GET", "HTTP_PROXY" => "http://evil:1" })
show("http://10.1.2.3/", { "REQUEST_METHOD" => "GET", "http_proxy" => "http://ok:1" })
show("http://10.1.2.3/", { "REQUEST_METHOD" => "GET", "CGI_HTTP_PROXY" => "http://cgi:1" })
begin
  URI("/rel").find_proxy({})
rescue URI::BadURIError => e
  p e.message
end

p URI::Generic.use_proxy?("www.example.com", nil, 80, "example.com")
p URI::Generic.use_proxy?("www.example.com", nil, 80, ".example.com")
p URI::Generic.use_proxy?("example.org", nil, 80, "example.com")
p URI::Generic.use_proxy?("example.com", nil, 443, "example.com:80")
p URI::Generic.use_proxy?("h", "192.168.1.5", 80, "192.168.0.0/16")

# URI's scheme classes hold their default port as DEFAULT_PORT, as CRuby's do.
require "uri"
p URI::Generic::DEFAULT_PORT, URI::HTTP::DEFAULT_PORT, URI::HTTPS::DEFAULT_PORT, URI::FTP::DEFAULT_PORT
p URI::WS::DEFAULT_PORT, URI::WSS::DEFAULT_PORT, URI::LDAP::DEFAULT_PORT, URI::LDAPS::DEFAULT_PORT
port = 443
puts "tls" if port == URI::HTTPS::DEFAULT_PORT
puts "plain" if port == URI::HTTP::DEFAULT_PORT
u = URI.parse("https://example.com/")
p u.port == URI::HTTPS::DEFAULT_PORT, u.default_port, URI::HTTP.default_port

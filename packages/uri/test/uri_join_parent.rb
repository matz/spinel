require "uri"

p URI.join("http://example.com/a/", "../c").to_s
p URI.join("https://example.com/a/b/", "../../c").to_s
p URI.join("http://example.com/a/b/", "../").to_s
p URI.join("http://example.com/a/b/", ".").to_s
p URI.join("http://example.com/a/b/", "..").to_s
p URI.join("http://example.com/a/", "c?x/../d").to_s

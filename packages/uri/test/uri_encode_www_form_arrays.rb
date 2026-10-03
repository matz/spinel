require "uri"

p URI.encode_www_form("tag" => ["red blue", "ruby&code"], "page" => 1)
p URI.encode_www_form([["tag", ["a", "b"]], ["tag", "c"]])
p URI.encode_www_form("empty" => [], "tail" => "ok")
p URI.encode_www_form("tag" => [nil, "x"], "flag" => nil)

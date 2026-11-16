# A String local an extension package's demand typed the handle, filled by
# an interpolation: `opaque = "#{opaque}?#{query}"` in URI's parser, in a
# program that also loads sqlite3. The interpolation builds its bytes, and
# the handle slot took a `const char *`: the C did not build. The slot takes
# a new handle around the bytes, as it takes any other fresh String.
require "uri"
require "sqlite3"

p URI.split("mailto:a@b?subject=hi")
p URI.split("http://h/p?q#f")

db = SQLite3::Database.new(":memory:")
db.execute("create table links (url text)")
db.execute("insert into links values (?)", ["https://example.com/a?b=1"])
db.execute("select url from links").each do |(url)|
  scheme, _, host, _, _, path, _, query, _ = URI.split(url)
  puts "#{scheme} #{host} #{path} #{query}"
end

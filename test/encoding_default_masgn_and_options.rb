# A multiple assignment's target sets Encoding.default_internal, and
# `encoding:` is ignored beside external_encoding: or internal_encoding:,
# which keep the external default. (CRuby warns that it ignores the
# `encoding:`; $VERBOSE = nil turns the warning off.)
require "tmpdir"
$VERBOSE = nil

p Encoding.default_internal
Encoding.default_internal, x = Encoding::UTF_8, 1
p Encoding.default_internal, x
Encoding.default_internal = nil

path = File.join(Dir.tmpdir, "spinel_encoding_default_masgn_#{Process.pid}.txt")
File.write(path, "héllo")
Encoding.default_external = Encoding::ASCII_8BIT
p File.read(path, encoding: "UTF-8", internal_encoding: "UTF-8").encoding
p File.read(path, encoding: "UTF-8", external_encoding: "BINARY").encoding
p File.open(path, encoding: "UTF-8", internal_encoding: "UTF-8") { |f| f.read.encoding }
p File.read(path, encoding: "UTF-8").encoding
Encoding.default_external = Encoding::UTF_8
p File.read(path, encoding: "BINARY", internal_encoding: "UTF-8").encoding
p File.read(path, 2, encoding: "UTF-8").encoding
File.delete(path)

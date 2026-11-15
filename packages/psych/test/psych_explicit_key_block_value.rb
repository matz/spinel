# A `? ` on a plain scalar's continuation line is the scalar's text (it
# hung the reader), and an explicit key's value may be a block collection
# that starts on the `:` line, as CRuby's emitter writes for a key that is
# no plain scalar.
require "yaml"
def try(s)
  p YAML.safe_load(s)
rescue => e
  puts "#{e.class}: #{e.message[0, 60]}"
end
try("k: x\n  - q\n")
try("k: x\n  ?q\n")
try("? a\n: b: c\n")
try("? a\n: - b\n")
try("x\n? q\n")
try("k: x\n  ? q\n")
try("k:\n  x\n  ? q\n")
try("? a\n: b: c\n  d: e\n")
try("? a\n: - b\n  - c\n? f\n: g\n")
try("? a\n:\n  b: c\n")
try("? [x, y]\n: v\n")
try("k: x\n  ? q\n  ? r\nm: n\n")
try("- x\n  ? q\n- y\n")
try("? k\n")
try("a: b\n? c\n: d: e\n")
h = { "multi\nline" => { "b" => "c" }, "plain" => [1, 2] }
p YAML.safe_load(h.to_yaml) == h

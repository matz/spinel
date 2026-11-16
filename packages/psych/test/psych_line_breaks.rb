# The line breaks as Psych reads them (libyaml's YAML 1.1): CR LF, CR and
# U+0085 are a newline, and U+2028 and U+2029 are breaks kept as themselves
# where a newline would fold to a space or be copied.
require "yaml"
def cps(s)
  v = YAML.safe_load(s)
  v = v["k"] if v.is_a?(Hash)
  p v.to_s.codepoints
rescue => e
  puts "#{e.class}: #{e.message[0, 60]}"
end
cps("k: 'yes   '\n")
cps("k: |-\n  yes \n")
cps("k: \"a \n  b\"\n")
cps("k: \"a\\L\"\n")
cps("k: 'a '\n")
cps("k: a b\n")
cps("k: 'a b'\n")
cps("k: \"a b\"\n")
cps("k: |\n  a b\n")
cps("k: >\n  a b\n")
cps("k: 'a   b'\n")
cps("k: 'a\u0085  b'\n")
def show(s)
  p YAML.safe_load(s)
rescue => e
  puts "#{e.class}: #{e.message[0, 90]}"
end
show("a: 1\r\nb: 2\r\n")
show("a: 1\rb: 2\r")
show("k: |\r\n  x\r\n  y\r\n")
show("k: >\r\n  x\r\n  y\r\n\r\n  z\r\n")
show("k: 'a\r\n  b'\r\n")
show("k: \"a\r\n\r\n  b\"\r\n")
show("k: a\r\n  b\r\n\r\n  c\r\n")
show("a: 1\u0085b: 2\u0085")
show("a: 1\u2028b: 2\n")
show("k: |+\n  yes\u2028\n\n")
show("k: |\n  yes\u2028\n")
show("k: >\n  a\u2028  b\n  c\n")
show("- a\u2029- b\n")
show("k: [a,\u2028 b]\n")
show("a: 1\r\nb: [\r\n")
show("a:\r\n  - x\r\n  y: z\r\n")
show("k: \"x\\\r\n  y\"\r\n")
show("# c\u2028k: v\n")

# strip / lstrip / rstrip on a UTF-8 String holding an invalid byte raise as
# CRuby does: rstrip (and strip) refuse a broken String wherever the byte sits
# (Encoding::CompatibilityError); lstrip raises ArgumentError only when the
# first character it does not strip is the invalid one. A binary String has
# no invalid bytes and strips as before.
def t(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue StandardError => e
  puts "#{label}: #{e.class.name} #{e.message}"
end

t("strip right edge") { " x\x81 ".strip }
t("strip left edge") { " \x81x ".strip }
t("strip middle") { " x\x81y ".strip }
t("strip only invalid") { "\x81".strip }
t("rstrip right edge") { " x\x81 ".rstrip }
t("rstrip left edge") { " \x81x ".rstrip }
t("rstrip truncated char") { "ab\xE3\x81".rstrip }
t("lstrip left edge") { " \x81x ".lstrip }
t("lstrip later byte") { " x\x81 ".lstrip }
t("lstrip after a char") { " ax\x81".lstrip }

s = +" x\x81 "
t("strip!") { s.strip! }
p s
t("rstrip!") { (+"x\x81").rstrip! }
t("lstrip!") { (+"\x81x").lstrip! }

t("strip chars") { " x\x81 ".strip(" ") }
t("strip chars left") { " \x81x ".strip(" ") }
t("lstrip chars stripped first") { "x\x81y".lstrip("x") }
t("lstrip chars later byte") { "ax\x81".lstrip("a") }

vals = [" y\x81 ", "ok "]
vals.each { |v| t("boxed") { v.strip } }

d = [0x20, 0x41, 0xff, 0x20].pack("C*").force_encoding("UTF-8")
t("built rstrip") { d.rstrip }
t("built lstrip") { d.lstrip }

t("binary strip") { " x\x81 ".b.strip }
t("binary rstrip") { "x\x81".b.rstrip }
t("binary strip chars") { " x\x81 ".b.strip(" ") }
t("valid multibyte") { "あ \t\n\0".rstrip.bytes }

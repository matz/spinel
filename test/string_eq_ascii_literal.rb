# `==` and `!=` against an ASCII string literal compare bytes inline: a
# string that matches only up to an embedded NUL, a BINARY string of the
# same bytes, strings the runtime hands back, and the literal on either side.
def show(label, v)
  puts "#{label}: #{v.inspect}"
end

s = "abc"
show "literal", s == "abc"
show "left", "abc" == s
show "shorter", s == "ab"
show "longer", s == "abcd"
show "differs", s == "abd"
show "ne", s != "abc"
show "ne other", s != "xyz"

built = "a" + "bc"
show "built", built == "abc"
interp = "#{"a"}b#{"c"}"
show "interpolated", interp == "abc"
m = +"ab"
m << "c"
show "mutated", m == "abc"
m << "d"
show "mutated longer", m == "abc"
show "sliced", "xabcx"[1, 3] == "abc"
show "joined", ["a", "b", "c"].join == "abc"
show "frozen", "abc".freeze == "abc"
show "upcased", "ABC".downcase == "abc"

nul = "abc\0def"
show "embedded NUL vs prefix", nul == "abc"
show "embedded NUL slice", nul[0, 3] == "abc"
show "embedded NUL tail", nul[4, 3] == "def"
show "NUL char", "\0" == "\0"
show "NUL vs empty", "\0" == ""

bin = "abc".b
show "binary same bytes", bin == "abc"
show "binary left", "abc" == bin
show "binary differs", bin == "abd"
show "utf8 literal vs binary", "é".b == "é"

e = ""
show "empty", e == ""
show "empty vs x", e == "x"
show "x vs empty", "x" == ""

show "nil.to_s", nil.to_s == ""
show "true.to_s", true.to_s == "true"
show "false.to_s", false.to_s == "true"
show "Integer#to_s", 42.to_s == "42"
show "Float#to_s", 1.5.to_s == "1.5"
show "Symbol#to_s", :abc.to_s == "abc"
show "class name", 1.class.name == "Integer"
show "ENV unset", ENV["SPINEL_STR_EQ_LIT_UNSET"] == "1"
ENV["SPINEL_STR_EQ_LIT_SET"] = "1"
show "ENV set", ENV["SPINEL_STR_EQ_LIT_SET"] == "1"

def control?(c)
  c == "\0" || c == "\r" || c == "\n" || c == "\x01" || c == "\x7f"
end
show "controls", (0..127).count { |b| control?(b.chr) }

words = %w[GET HEAD POST get GETS GE]
show "methods", words.map { |w| w == "GET" || w == "HEAD" }

def verb(m)
  case m
  when "GET", "HEAD" then :read
  when "POST" then :write
  when "" then :empty
  else :other
  end
end
show "case", ["GET", "HEAD", "POST", "", "get", "GETS", "GET\0X", "POST".b, +"PO" << "ST"].map { |m| verb(m) }

other = "PUT"
def verb2(m, other)
  case m
  when other then :var
  when "PUT\0" then :nul_lit
  when "é" then :utf8
  when "PUT" then :lit
  else :none
  end
end
show "case mixed", ["PUT", "PUT\0", "é", "é".b, "PU"].map { |m| verb2(m, other) }

def kind(s)
  case s
  in "json" then :json
  in "html" | "htm" then :html
  in String then :text
  end
end
show "pattern", ["json", "html", "htm", "txt", "json".b, "js"].map { |s| kind(s) }

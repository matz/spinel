# spinel: share
# spinel: gc-stress
# Follow-up regression tests for PR #12454 review findings:
# 1. Regexp non-ASCII \u escapes infer FIXEDENCODING / UTF-8 and reject /n.
# 2. Complex#fdiv invokes custom #coerce on boxed receivers.
# 3. MatchData#deconstruct_keys stops at the first unknown key.
# 4. Regexp /e, /s, /u keep distinct encodings and literal interning slots.

def poly(x) = [x, +"s"][0]

def check_err(label)
  yield
  puts "#{label}: NO_ERROR"
rescue Exception => e
  puts "#{label}: #{e.class}"
end

# --- 1. Regexp \u escape fixed_encoding? / encoding / options ---
def show_re(label, re)
  p [label, re.encoding.name, re.fixed_encoding?, re.options,
     poly(re).encoding.name, poly(re).fixed_encoding?, poly(re).options]
end

show_re(:lit_u_brace, /\u{6666}/)
show_re(:lit_u_4hex, /\u6666/)
show_re(:lit_u_ascii_brace, /\u{41}/)
show_re(:lit_u_ascii_4hex, /\u0041/)
show_re(:new_u_brace, Regexp.new('\u{6666}'))
show_re(:new_u_4hex, Regexp.new('\u6666'))
show_re(:new_u_cc, Regexp.new('[\u{6666}]'))
show_re(:new_u_list, Regexp.new('\u{41 6666}'))
show_re(:new_u_ascii_brace, Regexp.new('\u{41}'))
show_re(:new_u_ascii_4hex, Regexp.new('\u0041'))
show_re(:new_u_ascii_noenc, Regexp.new('\u{41}', Regexp::NOENCODING))
show_re(:new_u_comment_x, Regexp.new("# \\u{6666}\na", Regexp::EXTENDED))
show_re(:lit_linebreak, /\R/)
show_re(:new_linebreak_noenc, Regexp.new('\R', Regexp::NOENCODING))
check_err(:new_u_non_ascii_noenc) { Regexp.new('\u{6666}', Regexp::NOENCODING) }
check_err(:new_u_cc_non_ascii_noenc) { Regexp.new('[\u{6666}]', Regexp::NOENCODING) }

# --- 2. Complex#fdiv with custom #coerce ---
class FdivCoerceSame
  def coerce(other)
    [other, Complex(2, 0)]
  end
end

class FdivCoerceSwap
  def coerce(other)
    [Complex(2, 0), other]
  end
end

class FdivNoCoerce
end

p poly(Complex(4, 8)).fdiv(FdivCoerceSame.new)
p poly(Complex(4, 8)).fdiv(FdivCoerceSwap.new)
p Complex(4, 8).fdiv(FdivCoerceSame.new)
p Complex(4, 8).fdiv(FdivCoerceSwap.new)
p Complex(4, 8).fdiv(poly(FdivCoerceSame.new))
p Complex(4, 8).fdiv(poly(FdivCoerceSwap.new))
p Complex(4, 8).fdiv(2r)
check_err(:complex_fdiv_no_coerce) { poly(Complex(4, 8)).fdiv(FdivNoCoerce.new) }
check_err(:complex_fdiv_typed_no_coerce) { Complex(4, 8).fdiv(FdivNoCoerce.new) }

# --- 3. MatchData#deconstruct_keys unknown-key early stop ---
m = /(?<a>a)(?<b>b)(?<c>c)/.match("abc")
mb = poly(m)

p m.deconstruct_keys([:missing, :b])
p mb.deconstruct_keys([:missing, :b])
p m.deconstruct_keys([:a, :missing, :b])
p mb.deconstruct_keys([:a, :missing, :b])
p m.deconstruct_keys([:a, :missing])
p mb.deconstruct_keys([:a, :missing])
p m.deconstruct_keys([:missing, "x"])
p mb.deconstruct_keys([:missing, "x"])
p m.deconstruct_keys([:a, :b, :c, "x"])
p mb.deconstruct_keys([:a, :b, :c, "x"])
p m.deconstruct_keys([:b, :a])
p mb.deconstruct_keys([:b, :a])
p m.deconstruct_keys(nil)
p mb.deconstruct_keys(nil)
check_err(:md_dk_str_typed) { m.deconstruct_keys([:a, "x"]) }
check_err(:md_dk_str_boxed) { mb.deconstruct_keys([:a, "x"]) }

# --- 4. Regexp /e, /s, /u distinct encoding identity & literal interning ---
p [(/a/e).encoding.name, (/a/e).fixed_encoding?, (/a/e).options]
p [(/a/s).encoding.name, (/a/s).fixed_encoding?, (/a/s).options]
p [(/a/u).encoding.name, (/a/u).fixed_encoding?, (/a/u).options]
p [(/a/n).encoding.name, (/a/n).fixed_encoding?, (/a/n).options]
p [(/a/).encoding.name, (/a/).fixed_encoding?, (/a/).options]

re_e = /a/e
re_s = /a/s
re_u = /a/u
re_n = /a/n
re_plain = /a/

p [re_e.encoding == Encoding::EUC_JP, re_s.encoding == Encoding::Windows_31J,
   re_u.encoding == Encoding::UTF_8, re_n.encoding == Encoding::US_ASCII]
p [poly(re_e).encoding == Encoding::EUC_JP, poly(re_s).encoding == Encoding::Windows_31J,
   poly(re_u).encoding == Encoding::UTF_8, poly(re_n).encoding == Encoding::US_ASCII]
p [re_e == re_u, re_s == re_u, re_e == re_s, re_e == re_e, re_s == re_s, re_n == re_plain]
p [re_e.eql?(re_u), re_s.eql?(re_u), re_e.eql?(re_s), re_e.eql?(Regexp.new(re_e))]
p [poly(re_e) == poly(re_u), poly(re_s) == poly(re_u), poly(re_e) == poly(re_s)]
p [Regexp.new(re_e).encoding.name, Regexp.new(re_s).encoding.name]
p [Regexp.new(poly(re_e)).encoding.name, Regexp.new(poly(re_s)).encoding.name]
p ({ /a/e => 1, /a/s => 2, /a/u => 3, /a/ => 4 }).size

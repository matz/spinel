# spinel: share
# spinel: gc-stress
# Boxed runtime dispatch and respond_to? for the Regexp, MatchData, Complex,
# and Rational methods formerly listed in tools/respond_to_audit.rb's
# BOXED_UNCALLABLE.

def poly(x) = [x, +"s"][0]

def check_err(label)
  yield
  puts "#{label}: NO_ERROR"
rescue Exception => e
  puts "#{label}: #{e.class}"
end

# --- respond_to? on boxed receivers ---
re_box = poly(/a/)
md_box = poly(/(?<a>\w+)-(?<b>\d+)?/.match("foo-"))
cx_box = poly(1 + 2i)
rt_box = poly(3r)

p [
  re_box.respond_to?(:encoding),
  re_box.respond_to?(:fixed_encoding?),
  re_box.respond_to?(:timeout),
  re_box.respond_to?(:~),
]
p [
  md_box.respond_to?(:bytebegin),
  md_box.respond_to?(:byteend),
  md_box.respond_to?(:deconstruct),
  md_box.respond_to?(:deconstruct_keys),
  md_box.respond_to?(:match),
]
p [cx_box.respond_to?(:fdiv), cx_box.respond_to?(:clamp)]
p [rt_box.respond_to?(:clamp), rt_box.respond_to?(:to_c)]

# --- Regexp#encoding, #fixed_encoding?, #timeout, #~ ---
p [poly(/a/).encoding, poly(/a/u).encoding, poly(/a/n).encoding, poly(/é/).encoding]
p [poly(/a/).fixed_encoding?, poly(/a/u).fixed_encoding?, poly(/a/n).fixed_encoding?, poly(/é/).fixed_encoding?]
p [poly(Regexp.new("é")).encoding, poly(Regexp.new("é")).fixed_encoding?]
p [poly(/a/).timeout, poly(/a/i).timeout]
check_err(:enc_int) { poly(1).encoding }
check_err(:fixed_str) { poly("a").fixed_encoding? }
check_err(:fixed_int) { poly(1).fixed_encoding? }
check_err(:timeout_int) { poly(1).timeout }

$_ = "hello world"
p ~poly(/wor/)
p $~[0]
p ~poly(/zzz/)
p $~
$_ = "café world"
p ~poly(/wor/)
r_var = /wor/
p ~r_var
$_ = nil
p ~poly(/wor/)
$_ = 123
p ~poly(/wor/)
p ~poly(5)
check_err(:tilde_str) { ~poly("abc") }
check_err(:tilde_nil) { ~poly(nil) }
check_err(:tilde_flt) { ~poly(1.5) }

# --- MatchData#bytebegin, #byteend, #deconstruct, #deconstruct_keys, #match ---
md = /(?<a>\w+)-(?<b>\d+)?/.match("foo-")
bm = poly(md)
p [bm.bytebegin(0), bm.bytebegin(1), bm.bytebegin(2), bm.bytebegin(:a), bm.bytebegin("a"), bm.bytebegin(:b)]
p [bm.byteend(0), bm.byteend(1), bm.byteend(2), bm.byteend(:a), bm.byteend("a"), bm.byteend(:b)]

# UTF-8 multibyte byte offsets + boxed selector
um = poly(/(?<w>\w+)/.match("éé foo"))
k_sym = poly(:w)
k_str = poly("w")
k_int = poly(1)
p [um.bytebegin(:w), um.byteend(:w), um.bytebegin(k_sym), um.byteend(k_str), um.bytebegin(k_int)]

check_err(:bb_neg) { bm.bytebegin(-1) }
check_err(:bb_oob) { bm.bytebegin(99) }
check_err(:bb_bad) { bm.bytebegin(:nope) }
check_err(:bb_nil) { bm.bytebegin(nil) }
check_err(:bb_recv) { poly(1).bytebegin(0) }
check_err(:be_neg) { bm.byteend(-1) }
check_err(:be_oob) { bm.byteend(99) }
check_err(:be_bad) { bm.byteend(:nope) }
check_err(:be_nil) { bm.byteend(nil) }
check_err(:be_recv) { poly(1).byteend(0) }

# deconstruct
p bm.deconstruct
p poly(/a/.match("a")).deconstruct
p poly([1, 2]).deconstruct
check_err(:dec_int) { poly(1).deconstruct }
check_err(:dec_nil) { poly(nil).deconstruct }

# deconstruct_keys
p bm.deconstruct_keys(nil)
p bm.deconstruct_keys([:a])
p bm.deconstruct_keys([:a, :c])
p bm.deconstruct_keys([:a, :b, :c])
check_err(:dk_type) { bm.deconstruct_keys(1) }
check_err(:dk_elem) { bm.deconstruct_keys(["a"]) }
check_err(:dk_recv) { poly(1).deconstruct_keys(nil) }

# match
p [bm.match(0), bm.match(1), bm.match(2), bm.match(:a), bm.match("a"), bm.match(:b)]
p [um.match(k_sym), um.match(k_str), um.match(k_int)]
p [poly(/a/).match("a")[0], poly("a").match(/a/)[0]]
p [poly(/a/).match("qa")&.begin(0), poly(/a/).match("b")&.begin(0), poly(/(a)?/).match("")&.bytebegin(1)]
check_err(:md_match_neg) { bm.match(-1) }
check_err(:md_match_oob) { bm.match(99) }
check_err(:md_match_bad) { bm.match(:nope) }
check_err(:md_match_nil) { bm.match(nil) }
check_err(:md_match_arity) { bm.match(0, 1) }

# --- Complex#fdiv ---
p poly(1 + 2i).fdiv(2)
p poly(1 + 2i).fdiv(0.5)
p poly(1 + 2i).fdiv(1 / 2r)
p poly(1 + 2i).fdiv(1 + 1i)
p poly(1 + 2i).fdiv(0)
p [poly(1).fdiv(2), poly(3r).fdiv(2)]
check_err(:cpx_fdiv_str) { poly(1 + 2i).fdiv("x") }
check_err(:cpx_fdiv_nil) { poly(1 + 2i).fdiv(nil) }
check_err(:fdiv_recv) { poly("x").fdiv(2) }

# --- Rational#clamp ---
p [poly(3r).clamp(1, 2), poly(3r).clamp(4, 5), poly(3r).clamp(1r, 5r)]
p [poly(3r).clamp(nil, 2), poly(3r).clamp(4, nil), poly(3r).clamp(nil, nil)]
p [poly(3r).clamp(1..2), poly(3r).clamp(4..), poly(3r).clamp(..2), poly(3r).clamp(1.5..2.5)]
r_int = 1..2
r_flt = 1.5..2.5
r_box = poly(1..2)
p [poly(3r).clamp(r_int), poly(3r).clamp(r_flt), poly(3r).clamp(r_box)]
check_err(:rat_clamp_excl) { poly(3r).clamp(1...2) }
r_excl = 1...2
check_err(:rat_clamp_excl_var) { poly(3r).clamp(r_excl) }
check_err(:rat_clamp_order) { poly(3r).clamp(5, 1) }
check_err(:rat_clamp_cmp) { poly(3r).clamp(1, "x") }
check_err(:rat_clamp_nonrange) { poly(3r).clamp(1) }
check_err(:rat_clamp_nilrange) { poly(3r).clamp(nil) }
check_err(:cpx_clamp) { poly(1 + 2i).clamp(1, 2) }

# --- Rational#to_c ---
c1 = poly(3r).to_c
c2 = poly(3 / 2r).to_c
p [c1.class, c1 == Complex(3, 0), c1.real == 3, c1.imag == 0]
p [c2.class, c2 == (3 / 2r).to_c, c2.real == 1.5, c2.imag == 0]
p [poly(nil).to_c, poly(1 + 2i).to_c, poly("3+4i").to_c]
check_err(:toc_arr) { poly([1]).to_c }

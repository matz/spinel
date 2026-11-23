# spinel: share
# spinel: gc-minor
# spinel: gc-stress
# StringScanner#[] named captures (String and Symbol keys), #matched_size,
# #captures, #named_captures, and #values_at matching CRuby 4.0.
require "strscan"

def capture_err
  yield
rescue IndexError => e
  [:IndexError, e.message]
rescue TypeError => e
  [:TypeError, e.message]
end

# 1. Before any match
s = StringScanner.new("foo=123 bar=456")
p [:pre_aref, s[0], s[1], s["k"], s[:k], s[nil], s[[]]]
p [:pre_helpers, s.matched_size, s.captures, s.named_captures, s.values_at, s.values_at(0, :k, "missing")]

# 2. Named captures via scan
s.scan(/(?<k>\w+)=(?<v>\d+)/)
p [:scan_int, s[0], s[1], s[2], s[-1], s[-3], s[-4], s[5]]
p [:scan_name, s["k"], s[:k], s["v"], s[:v]]
p [:scan_missing, capture_err { s["missing"] }, capture_err { s[:missing] }]
p [:scan_bad_arg, capture_err { s[nil] }, capture_err { s[[]] }]
p [:scan_poly_keys, [0, :k, "v", -1, 1.9, 9].map { |idx| s[idx] }]
p [:scan_size, s.matched_size]
p [:scan_captures, s.captures]
p [:scan_named, s.named_captures]
p [:scan_values_at, s.values_at(0, :k, "v", -1, 5), s.values_at]
keys = [:k, "v", 0]
p [:scan_values_at_splat, s.values_at(*keys)]
p [:scan_values_at_missing, capture_err { s.values_at(0, "missing") }]

# 3. Freshness and --share-strings aliasing of named captures
a = s[:k]
b = a
b << "!"
p [:share_sym, a, b, a.equal?(b), s[:k]]
c = s["v"]
c << "?"
p [:share_str, c, s["v"]]
h1 = s.named_captures
h2 = s.named_captures
c1 = s.captures
c2 = s.captures
p [:fresh_containers, h1.equal?(h2), c1.equal?(c2), c1[0].equal?(c2[0])]

# 4. Repeated capture names and optional non-participating groups
sr1 = StringScanner.new("abc")
sr1.scan(/(?<x>a)(?<x>b)?(?<y>z)?/)
p [:rep1, sr1["x"], sr1[:x], sr1["y"], sr1.captures, sr1.named_captures]

sr2 = StringScanner.new("ac")
sr2.scan(/(?<x>a)(?:(?<x>b)|(?<z>c))/)
p [:rep2, sr2["x"], sr2["z"], sr2.captures, sr2.named_captures]

sr3 = StringScanner.new("ab")
sr3.scan(/(?<x>a)(?<x>b)/)
p [:rep3, sr3["x"], sr3.captures, sr3.named_captures]

sr4 = StringScanner.new("c")
sr4.scan(/(?<x>a)?(?<x>b)?c/)
p [:rep4, sr4["x"], sr4.captures, sr4.named_captures]

# 5. All match-setting methods (match?, check, skip, exist?, check_until, skip_until, scan_until)
sm = StringScanner.new("日本語=123!end")
sm.match?(/(?<w>日本語)=(?<n>\d+)/)
p [:match_q, sm.matched_size, sm[:w], sm["n"], sm.captures, sm.named_captures]
sm.check(/(?<w>日本語)=/)
p [:check, sm.matched_size, sm[:w], sm.captures, sm.named_captures]
sm.skip(/(?<w>日本語)=/)
p [:skip, sm.matched_size, sm[:w], sm.captures, sm.named_captures]
sm.exist?(/(?<d>\d+)(?<bang>!)/)
p [:exist_q, sm.matched_size, sm[:d], sm[:bang], sm.named_captures]
sm.check_until(/(?<d>\d+)(?<bang>!)/)
p [:check_until, sm.matched_size, sm[:d], sm[:bang], sm.named_captures]
sm.skip_until(/(?<d>\d+)(?<bang>!)/)
p [:skip_until, sm.matched_size, sm[:d], sm[:bang], sm.named_captures]
sm.scan_until(/(?<tail>end)/)
p [:scan_until, sm.matched_size, sm[:tail], sm.named_captures]

# 6. Empty match and \K matched_size
se = StringScanner.new("abcdef")
se.scan(//)
p [:empty_match, se.matched?, se.matched_size, se.captures, se.named_captures]
se.scan(/ab\Kcd/)
p [:keep_match, se.matched, se.matched_size, se[0], se.captures]

# 7. String-literal pattern, getch, get_byte, scan_byte
sl0 = StringScanner.new("hello world")
sl0.scan("hello")
p [:fresh_lit, sl0.matched_size, sl0[0], sl0[1], sl0.captures, sl0.named_captures, sl0.values_at(0, 1)]
sg0 = StringScanner.new("日本語")
sg0.getch
p [:fresh_getch, sg0.matched_size, sg0[0], sg0.captures, sg0.named_captures]

sl = StringScanner.new("hello world")
sl.scan(/(?<h>hello)/)
sl.scan(" ")
p [:lit_match, sl.matched_size, sl[0], sl[1], sl.captures, sl.named_captures, sl.values_at(0, 1), capture_err { sl[:h] }]

sg = StringScanner.new("日本語")
sg.scan(/(?<j>日)/)
sg.getch
p [:getch_match, sg.matched_size, sg[0], sg[1], sg.captures, sg.named_captures, capture_err { sg[:j] }]
sg.get_byte
p [:get_byte_match, sg.matched_size, sg.captures, sg.named_captures]

# 8. State transitions: failed match, pos=, unscan, terminate, reset, string=, dup/clone
st = StringScanner.new("a=1 b=2")
st.scan(/(?<k>\w+)=(?<v>\d+)/)
st.pos = 0
p [:pos_keeps_match, st.matched?, st.matched_size, st[:k], st.named_captures]
d = st.dup
p [:dup_state, d.matched_size, d.captures, d.named_captures, capture_err { d[:k] }]
st.unscan
p [:after_unscan, st.matched?, st.matched_size, st[:k], st.captures, st.named_captures, st.values_at(0)]
st.scan(/(?<k>\w+)=(?<v>\d+)/)
st.scan(/NOMATCH/)
p [:after_miss, st.matched?, st.matched_size, st[:k], st.captures, st.named_captures, st.values_at(0)]
st.scan(/(?<k>\w+)=(?<v>\d+)/)
st.terminate
p [:after_term, st.matched?, st.matched_size, st.captures, st.named_captures]
st.reset
p [:after_reset, st.matched?, st.matched_size, st.captures, st.named_captures]
st.scan(/(?<k>\w+)=(?<v>\d+)/)
st.string = "z=9"
p [:after_str_set, st.matched?, st.matched_size, st.captures, st.named_captures]

# 9. Binary (ASCII-8BIT) encoding preservation
sb = StringScanner.new("foo=bar".b)
sb.scan(/(?<k>\w+)=(?<v>\w+)/)
p [:bin_enc, sb.matched.encoding.name, sb[0].encoding.name, sb["k"].encoding.name, sb[:v].encoding.name,
   sb.captures.map { |x| x.encoding.name }, sb.values_at(0, :k, "v").map { |x| x.encoding.name },
   sb.named_captures["k"].encoding.name]

# 10. Custom #to_int index object
class IdxBox
  def initialize(n)
    @n = n
  end
  def to_int
    @n
  end
end
si = StringScanner.new("foo=42")
si.scan(/(?<k>\w+)=(?<v>\d+)/)
p [:to_int, si[IdxBox.new(1)], si.values_at(IdxBox.new(2), :k)]

# 11. GC stress loop with dynamically constructed strings and capture keys
acc = 0
200.times do |i|
  src = "key#{i}=val#{i}"
  sc = StringScanner.new(src)
  sc.scan(/(?<key>\w+)=(?<val>\w+)/)
  kname = "ke" + "y"
  vname = "va" + "l"
  vals = sc.values_at(kname, :val, 0)
  nc = sc.named_captures
  caps = sc.captures
  acc += sc.matched_size + vals[0].bytesize + nc[vname].bytesize + caps[1].bytesize
end
p [:gc_stress_acc, acc]

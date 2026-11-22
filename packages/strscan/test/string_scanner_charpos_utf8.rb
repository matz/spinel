# spinel: share
# spinel: gc-stress
# StringScanner#charpos returns character offsets on UTF-8 input (matching
# CRuby) rather than byte offsets, uses an O(1) fast path for binary and
# ASCII-only strings, and maintains its checkpoint across forward scans,
# mid-character pos=, backward pos=, unscan, reset, terminate, dup, and string=.
require "strscan"

# 1. Basic multibyte UTF-8 forward scanning
s = StringScanner.new("日本語abc")
p [:init, s.pos, s.charpos]
s.scan(/日本語/)
p [:after_jp, s.pos, s.charpos, s.charpos]
s.scan(/ab/)
p [:after_ab, s.pos, s.charpos]
s.terminate
p [:after_term, s.pos, s.charpos]
s.reset
p [:after_reset, s.pos, s.charpos]

# 2. 2-byte, 3-byte, and 4-byte UTF-8 characters with all scan/skip/check/getch ops
u = StringScanner.new("café€😀z")
p [:u_check, u.check(/café/), u.pos, u.charpos]
p [:u_match_q, u.match?(/café/), u.pos, u.charpos]
p [:u_skip, u.skip(/café/), u.pos, u.charpos]
p [:u_getch1, u.getch, u.pos, u.charpos]
p [:u_getch2, u.getch, u.pos, u.charpos]
u.unscan
p [:u_unscan, u.pos, u.charpos]
p [:u_scan_until, u.scan_until(/z/), u.pos, u.charpos]

# 3. Mid-character pos= and subsequent forward advances (checkpoint must not drift)
m = StringScanner.new("日本語abc")
[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 6, 3, 0, -3, -1].each do |p_off|
  m.pos = p_off
  p [:mid_jp, p_off, m.pos, m.charpos]
end

e = StringScanner.new("😀a")
[0, 1, 2, 3, 4, 5, 2, 4, 0].each do |p_off|
  e.pos = p_off
  p [:mid_emoji, p_off, e.pos, e.charpos]
end

# 4. ASCII-only and binary (ASCII-8BIT) strings
a = StringScanner.new("hello world")
a.scan(/hello/)
p [:ascii, a.pos, a.charpos]
a.pos = 2
p [:ascii_back, a.pos, a.charpos]

b = StringScanner.new("日本語abc".b)
[0, 1, 3, 6, 9, 12, 2].each do |p_off|
  b.pos = p_off
  p [:bin, p_off, b.pos, b.charpos]
end

# 5. Invalid UTF-8 byte sequences and embedded NULs
[
  "\xC0\xAF",
  "\xE0\x80\xAF",
  "\xED\xA0\x80",
  "\xF4\x90\x80\x80",
  "\xF0\x9F\x98",
  "\x80\x81",
  "a\x00b日本語\x00c",
].each_with_index do |str, idx|
  sc = StringScanner.new(str)
  vals = []
  i = 0
  while i <= str.bytesize
    sc.pos = i
    vals << sc.charpos
    i += 1
  end
  p [:inv, idx, vals]
end

# 6. string= and dup independence
r = StringScanner.new("日本語")
r.scan(/日/)
d = r.dup
p [:dup_before, r.charpos, d.charpos]
r.string = "αβγ"
r.scan(/αβ/)
p [:after_string_set, r.pos, r.charpos, d.pos, d.charpos]
d.scan(/本/)
p [:dup_after_scan, d.pos, d.charpos]

# 7. Repeated forward charpos calls across a longer mixed UTF-8 string
long_src = "αβγ日本語😀ascii" * 20
ls = StringScanner.new(long_src)
char_count = 0
while ls.getch
  char_count += 1
  raise "mismatch" unless ls.charpos == char_count
end
p [:long_loop, ls.pos, ls.charpos, char_count]

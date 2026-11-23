# spinel: share

def show(label)
  v = yield
  puts "#{label}: OK #{v.inspect}"
rescue NoMethodError => e
  puts "#{label}: NoMethodError (#{e.name})"
end

rd, wr = IO.pipe
wr.write("ab\ncd")
wr.close
lines = []
while (line = rd.gets)
  lines << line.strip
end
puts "io_loop: #{lines.inspect}"
eof_line = rd.gets
puts "io_gets_nil: #{eof_line.nil?} #{eof_line.class} #{eof_line.is_a?(String)} #{eof_line.respond_to?(:upcase)}"
show("io_gets_empty") { eof_line.empty? }
show("io_gets_upcase") { eof_line.upcase }
eof_ch = rd.getc
puts "io_getc_nil: #{eof_ch.nil?} #{eof_ch.respond_to?(:upcase)}"
show("io_getc_empty") { eof_ch.empty? }
eof_buf = rd.read(4)
puts "io_read_n_nil: #{eof_buf.nil?} #{eof_buf.respond_to?(:upcase)}"
show("io_read_n_empty") { eof_buf.empty? }
eof_all = rd.read
puts "io_read_all: #{eof_all.inspect} #{eof_all.empty?}"
rd.close

rd2, wr2 = IO.pipe
wr2.write("hi\n")
wr2.close
raw_lines = [rd2.gets, rd2.gets]
raw_lines.each_with_index do |s, idx|
  puts "raw_line_#{idx}: #{s.respond_to?(:upcase)} #{s.nil?}"
end
rd2.close

miss_env = ENV["SPINEL_TEST_DEFINITELY_MISSING_KEY_987654"]
puts "env_miss: #{miss_env.nil?} #{miss_env.class} #{miss_env.respond_to?(:upcase)}"
show("env_miss_empty") { miss_env.empty? }
env_del = ENV.delete("SPINEL_TEST_DEFINITELY_MISSING_KEY_987654")
puts "env_del: #{env_del.nil?} #{env_del.respond_to?(:upcase)}"
env_fetch_nil = ENV.fetch("SPINEL_TEST_DEFINITELY_MISSING_KEY_987654", nil)
puts "env_fetch_nil: #{env_fetch_nil.nil?} #{env_fetch_nil.respond_to?(:upcase)}"

t_utc = Time.utc(2020, 1, 1)
t_off = Time.new(2020, 1, 1, 0, 0, 0, "+02:00")
puts "time_utc_zone: #{t_utc.zone.respond_to?(:upcase)} #{t_utc.zone}"
puts "time_off_zone: #{t_off.zone.nil?} #{t_off.zone.respond_to?(:upcase)}"
show("time_off_zone_empty") { t_off.zone.empty? }

md = /(a)?(b)/.match("b")
g1 = md[1]
g2 = md[2]
m1 = md.match(1)
puts "md_aref: #{g1.nil?} #{g1.respond_to?(:upcase)} #{g2.respond_to?(:upcase)} #{g2}"
puts "md_match: #{m1.nil?} #{m1.respond_to?(:upcase)}"
show("md_aref_empty") { g1.empty? }
show("md_match_empty") { m1.empty? }

sym_hit = :hello[1, 2]
sym_miss = :hello[99, 2]
sym_sl = :hello.slice(99, 2)
puts "sym_slice: #{sym_hit} #{sym_miss.nil?} #{sym_miss.respond_to?(:upcase)} #{sym_sl.respond_to?(:upcase)}"
show("sym_miss_empty") { sym_miss.empty? }

s1 = +"abc"
puts "str_strip_nil: #{s1.strip!.nil?} #{s1.strip!.respond_to?(:upcase)}"
s2 = +"abc"
miss_slice = s2[10, 2]
puts "str_slice_miss: #{miss_slice.nil?} #{miss_slice.respond_to?(:upcase)}"
show("str_slice_empty") { miss_slice.empty? }

open_end = ("a"..)
open_beg = (.."z")
empty_sr = ("z"..."a")
puts "srange_open: #{open_end.end.nil?} #{open_end.end.respond_to?(:upcase)} #{open_beg.begin.nil?} #{open_beg.begin.respond_to?(:upcase)}"
show("srange_end_empty") { open_end.end.empty? }
puts "srange_empty_minmax: #{empty_sr.min.nil?} #{empty_sr.min.respond_to?(:upcase)} #{empty_sr.max.nil?} #{empty_sr.max.respond_to?(:upcase)}"

class Item
  attr_reader :v
  def initialize(v) = @v = v
end

sb = ["a", "b"].bsearch { |s| s >= "z" }
puts "str_bsearch: #{sb.nil?} #{sb.respond_to?(:upcase)}"
show("str_bsearch_empty") { sb.empty? }

nested_b = [[1], [2]].bsearch { |a| a[0] >= 9 }
puts "nested_bsearch: #{nested_b.nil?} #{nested_b.respond_to?(:size)}"
show("nested_bsearch_size") { nested_b.size }

items = [Item.new(1), Item.new(2)]
ib = items.bsearch { |x| x.v >= 9 }
puts "obj_bsearch: #{ib.nil?} #{ib.respond_to?(:v)}"
show("obj_bsearch_v") { ib.v }

strs_with_nil = ["ok", nil]
strs_with_nil.find_all { |_| true }.each do |s|
  puts "find_all_str: #{s.respond_to?(:upcase)}"
end
strs_with_nil.select { |_| true }.each do |s|
  puts "select_str: #{s.respond_to?(:upcase)}"
end
strs_with_nil.reject { |_| false }.each do |s|
  puts "reject_str: #{s.respond_to?(:upcase)}"
end
strs_with_nil.uniq.each do |s|
  puts "uniq_str: #{s.respond_to?(:upcase)}"
end
strs_with_nil.each_with_index do |s, _|
  puts "poly_blk: #{s.respond_to?(:upcase)}"
  show("poly_blk_empty") { s.empty? }
  s = 1 if false
end
sa_pop = ["ok", nil]
sa_pop.pop(2).each do |s|
  puts "pop_n_str: #{s.respond_to?(:upcase)}"
end
sa_shift = ["ok", nil]
sa_shift.shift(2).each do |s|
  puts "shift_n_str: #{s.respond_to?(:upcase)}"
end
["ok"].values_at(0, 5).each do |s|
  puts "values_at_str: #{s.respond_to?(:upcase)}"
end
(["ok"] * 1).each do |s|
  puts "mul_non_nil: #{s.upcase}"
end
(strs_with_nil * 1).each do |s|
  puts "mul_nil: #{s.respond_to?(:upcase)}"
end
["a"].flat_map { |s| [s, nil] }.each do |s|
  puts "flat_map_nil: #{s.respond_to?(:upcase)}"
end
strs_with_nil.compact.each do |s|
  puts "compact_str: #{s.upcase}"
end
strs_with_nil.filter_map { |s| s }.each do |s|
  puts "filter_map_str: #{s.upcase}"
end
["b", "a"].min(1) { |a, b| a <=> b }.each do |s|
  puts "min_n_blk: #{s.upcase}"
end

objs_with_nil = [Item.new(10), nil]
objs_with_nil.find_all { |_| true }.each do |o|
  show("obj_find_all") { o.v }
end
objs_with_nil.each_with_index do |o, _|
  show("obj_ewi") { o.v }
end
objs_pop = [Item.new(20), nil]
objs_pop.pop(2).each do |o|
  show("obj_pop_n") { o.v }
end
[Item.new(30)].values_at(0, 3).each do |o|
  show("obj_values_at") { o.v }
end

ok_strs = ["hello"]
puts "fetch_non_nil: #{ok_strs.fetch(0).upcase}"
puts "fetch_from_nil_arr: #{strs_with_nil.fetch(1).respond_to?(:upcase)}"
dflt_str = ok_strs.size > 10 ? "x" : nil
fetch_dflt = ok_strs.fetch(5, dflt_str)
puts "fetch_default_nil: #{fetch_dflt.respond_to?(:upcase)}"
show("fetch_default_empty") { fetch_dflt.empty? }

h = { "a" => "hello" }
puts "hash_miss: #{h["z"].nil?} #{h["z"].respond_to?(:upcase)}"
puts "hash_del: #{h.delete("z").nil?} #{h.delete("z").respond_to?(:upcase)}"
puts "hash_dig: #{h.dig("z").nil?} #{h.dig("z").respond_to?(:upcase)}"
puts "hash_assoc: #{h.assoc("z").nil?} #{h.assoc("z").respond_to?(:size)}"
puts "hash_rassoc: #{h.rassoc("z").nil?} #{h.rassoc("z").respond_to?(:size)}"
puts "hash_find: #{h.find { |k, _| k == "z" }.nil?} #{h.find { |k, _| k == "z" }.respond_to?(:size)}"
h_nil = { "a" => "hello", "n" => nil }
puts "hash_fetch_nil: #{h_nil.fetch("n").nil?} #{h_nil.fetch("n").respond_to?(:upcase)}"
show("hash_fetch_nil_empty") { h_nil.fetch("n").empty? }
h_empty = {}
h_empty["k"] = "v"
h_empty.clear
puts "hash_shift: #{h_empty.shift.nil?} #{h_empty.shift.respond_to?(:size)}"

h_nil.values.each do |s|
  puts "hash_values_nil: #{s.respond_to?(:upcase)}"
end

empty_sa = ["x"]
empty_sa.clear
empty_sa.minmax.each do |s|
  puts "arr_minmax_empty: #{s.nil?} #{s.respond_to?(:upcase)}"
end
empty_sa.minmax_by { |s| s }.each do |s|
  puts "arr_minmax_by_empty: #{s.nil?} #{s.respond_to?(:upcase)}"
end
empty_sr.minmax.each do |s|
  puts "srange_minmax_empty: #{s.nil?} #{s.respond_to?(:upcase)}"
end

rng = Random.new(1)
strs_with_nil.sample(2, random: rng).sort_by { |s| s.nil? ? "" : s }.each do |s|
  puts "sample_n_kw: #{s.respond_to?(:upcase)}"
end

strs_with_nil.grep_v(/ok/) { |s| puts "grep_v_blk: #{s.respond_to?(:upcase)}"; s }
strs_with_nil.rfind { |s| puts "rfind_blk: #{s.respond_to?(:upcase)}"; false }
maybe_memo = ok_strs.size > 10 ? "init" : nil
["a"].each_with_object(maybe_memo) do |s, m|
  puts "ewo_memo: #{s.upcase} #{m.nil?} #{m.respond_to?(:upcase)}"
end

sa_taken = ["only"]
puts "taken_str_pop1: #{sa_taken.pop.upcase}"
sp_empty = sa_taken.pop
puts "taken_str_pop2: #{sp_empty.nil?} #{sp_empty.respond_to?(:upcase)}"
show("taken_str_pop_empty") { sp_empty.empty? }
sa_taken2 = ["only"]
puts "taken_str_shift1: #{sa_taken2.shift.upcase}"
ss_empty = sa_taken2.shift
puts "taken_str_shift2: #{ss_empty.nil?} #{ss_empty.respond_to?(:upcase)}"
sd_miss = ["only"].delete("none")
sda_miss = ["only"].delete_at(5)
ssl_miss = ["only"].slice!(5)
puts "taken_str_del: #{sd_miss.nil?} #{sda_miss.nil?} #{ssl_miss.nil?}"

obj_taken = [Item.new(42)]
puts "taken_obj_pop1: #{obj_taken.pop.v}"
op_empty = obj_taken.pop
puts "taken_obj_pop2: #{op_empty.nil?} #{op_empty.respond_to?(:v)}"
show("taken_obj_pop_v") { op_empty.v }
obj_taken2 = [Item.new(43)]
puts "taken_obj_shift1: #{obj_taken2.shift.v}"
os_empty = obj_taken2.shift
show("taken_obj_shift_v") { os_empty.v }
od_miss = [Item.new(44)].delete(Item.new(99))
oda_miss = [Item.new(44)].delete_at(5)
osl_miss = [Item.new(44)].slice!(5)
show("taken_obj_del_v") { od_miss.v }
show("taken_obj_del_at_v") { oda_miss.v }
show("taken_obj_slice_bang_v") { osl_miss.v }

nullable_s = ok_strs.size > 10 ? "hi" : nil
dup_s = nullable_s.dup
tap_s = nullable_s.tap { |_| }
puts "recv_fwd: #{dup_s.nil?} #{tap_s.nil?} #{dup_s.respond_to?(:upcase)} #{tap_s.respond_to?(:upcase)}"

d = Dir.open(".")
while d.read
end
d_eof = d.read
d.close
puts "dir_read_eof: #{d_eof.nil?} #{d_eof.respond_to?(:upcase)}"

poly_recv = ok_strs.size > 10 ? 1 : ["a"]
poly_first = poly_recv[5]
puts "poly_recv_aref: #{poly_first.nil?} #{poly_first.respond_to?(:upcase)}"

["a", "b"].map { |s| next nil if s == "b"; s }.each do |s|
  puts "map_next_nil: #{s.respond_to?(:upcase)}"
end
(["a"] + [nil]).each do |s|
  puts "merge_plus: #{s.respond_to?(:upcase)}"
end
(["a"] | [nil]).each do |s|
  puts "merge_pipe: #{s.respond_to?(:upcase)}"
end
["a"].union([nil]).each do |s|
  puts "merge_union: #{s.respond_to?(:upcase)}"
end
strs_with_nil.dup.each do |s|
  puts "dup_elem: #{s.respond_to?(:upcase)}"
end
strs_with_nil.tap { |_| }.each do |s|
  puts "tap_elem: #{s.respond_to?(:upcase)}"
end

class CmpBox
  attr_reader :v
  def initialize(v) = @v = v
  def ==(other)
    return false if other.nil?
    other.respond_to?(:v) && @v == other.v
  end
  def <=>(other)
    return nil if other.nil?
    @v <=> other.v
  end
end
cb = CmpBox.new(1)
puts "cb_eq_nil: #{[cb].include?(nil)} #{cb == nil} #{(cb <=> nil).nil?}"

gap_strs = ["a"]
gap_strs[2] = "b"
[1, 2].flat_map { |i| i == 1 ? ["x"] : gap_strs }.each do |s|
  puts "flat_map_int_recv: #{s.nil?} #{s.respond_to?(:upcase)}"
end
[1, 2].map { |i| i == 1 ? "x" : nil }.each do |s|
  puts "map_int_recv: #{s.nil?} #{s.respond_to?(:upcase)}"
end
["a", "b"].flat_map { |s| next gap_strs if s == "b"; [s] }.each do |s|
  puts "flat_map_next_arr: #{s.nil?} #{s.respond_to?(:upcase)}"
end
["a", "b"].flat_map { |s| next [s] if s == "b"; [s] }.each do |s|
  puts "flat_map_next_clean: #{s.upcase}"
end

seed_arr = ["a"]
seed_arr[2] = "b"
[1].reduce(seed_arr) do |acc, _|
  acc.each { |s| puts "reduce_seed_arr: #{s.nil?} #{s.respond_to?(:upcase)}" }
  acc
end
class MiniEnum
  include Enumerable
  def each
    yield "x"
  end
end
seed_enum = ["a"]
seed_enum[2] = "b"
MiniEnum.new.reduce(seed_enum) do |acc, _|
  acc.each { |s| puts "reduce_seed_enum: #{s.nil?} #{s.respond_to?(:upcase)}" }
  acc
end
seed_hash = ["a"]
seed_hash[2] = "b"
{ "k" => 1 }.reduce(seed_hash) do |acc, _|
  acc.each { |s| puts "reduce_seed_hash: #{s.nil?} #{s.respond_to?(:upcase)}" }
  acc
end

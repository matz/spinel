# A call with no argument on a boxed receiver (a mixed local, an Array element, a
# top-level ivar, a parameter) is the Array's, Hash's or String's, as the typed
# receiver's is: Hash#merge, values_at, union, difference, intersection and zip,
# the blockless index, rindex, find_index, sort_by and bsearch, slice, and the
# String's start_with?, end_with?, dedup and encode!. A receiver that has no such
# method stays NoMethodError, and a program class of the name keeps its own.
def t(l)
  r = yield
  puts [l, r].inspect
rescue Exception => e
  puts [l, e.class].inspect
end
def mkh(i) = i == 0 ? {a: 1, b: 2} : [1]
def mka(i) = i == 0 ? [3, 1, 2] : {k: 1}
def mks(i) = i == 0 ? +"ab c" : [1]
t(:p01_loc) { mkh(0).merge }
e = {}; n = {z: 1}
t(:p01_kw_e) { mkh(0).merge(**e) }
t(:p01_kw_n) { mkh(0).merge(**n) }
ar = [{a: 1}, [1]]
t(:p01_elt) { ar[0].merge }
@iv = mkh(0)
t(:p01_iv) { @iv.merge }
t(:p01_arg) { mkh(0).merge({z: 1}) }
t(:p01_blk) { mkh(0).merge { |k, a, b| a } }
t(:p01_ary) { mkh(1).merge }
t(:p01_fresh) { h = mkh(0); h.merge.equal?(h) }
t(:p02_arr) { mka(0).values_at }
t(:p02_hash) { mka(1).values_at }
t(:p02_one) { mka(0).values_at(0, 2) }
t(:p02_str) { mks(0).values_at }
t(:p03_union) { mka(0).union }
t(:p03_diff) { mka(0).difference }
t(:p03_inter) { mka(0).intersection }
t(:p03_union_h) { mka(1).union }
t(:p03_union_arg) { mka(0).union([9]) }
t(:p03_union_s) { mks(0).union }
t(:p04_arr) { mka(0).zip }
t(:p04_hash) { mka(1).zip }
t(:p04_str) { mks(0).zip }
t(:p04_arg) { mka(0).zip([1, 2, 3]) }
t(:p05_index) { mka(0).index }
t(:p05_rindex) { mka(0).rindex }
t(:p05_find_index) { mka(0).find_index }
t(:p05_s_index) { mks(0).index }
t(:p05_s_rindex) { mks(0).rindex }
t(:p05_h_index) { mka(1).index }
t(:p05_with_arg) { mka(0).index(1) }
t(:p05_with_blk) { mka(0).index { |x| x == 1 } }
t(:p06_sort_by) { mka(0).sort_by }
t(:p06_sort_by_h) { mka(1).sort_by }
t(:p06_bsearch) { mka(0).bsearch }
t(:p06_sort_by_blk) { mka(0).sort_by { |x| -x } }
t(:p06_sort_by_s) { mks(0).sort_by }
t(:p07_arr) { mka(0).slice }
t(:p07_str) { mks(0).slice }
t(:p07_hash) { mka(1).slice }
t(:p07_arr_arg) { mka(0).slice(1) }
t(:p07_str_arg) { mks(0).slice(1, 2) }
t(:p08_sw) { mks(0).start_with? }
t(:p08_ew) { mks(0).end_with? }
t(:p08_dedup) { mks(0).dedup }
t(:p08_enc) { mks(0).encode! }
t(:p08_sw_arr) { mka(0).start_with? }
t(:p08_sw_arg) { mks(0).start_with?("ab") }
t(:p09_s_merge) { mks(0).merge }
t(:p09_a_merge) { mkh(1).merge }
t(:p09_h_union) { mka(1).union }
t(:p09_s_values_at) { mks(0).values_at }
t(:p09_h_difference) { mka(1).difference }
t(:p09_a_start) { mka(0).start_with? }
x = nil
t(:p13_nil_merge) { x&.merge }
t(:p13_merge) { mkh(0)&.merge }
t(:p13_values_at) { mka(0)&.values_at }
t(:p13_union) { mka(0)&.union }
t(:p13_start) { mks(0)&.start_with? }

# a boxed Range has zip and sort_by, a boxed Hash find_index (Range#find_index
# stays NoMethodError for a boxed Range)
def mr(i) = i == 0 ? (1..3) : i == 1 ? {a: 1} : [3, 1, 2]
t(:range_zip) { mr(0).zip }
t(:range_sort_by) { mr(0).sort_by.class }
t(:hash_find_index) { mr(1).find_index.class }
t(:hash_zip) { mr(1).zip }
t(:arr_zip) { mr(2).zip }

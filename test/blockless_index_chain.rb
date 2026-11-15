# A blockless index, rindex or find_index with no argument is an Enumerator also
# where it is the receiver of a call with a block: on a typed Array, on a boxed
# Array, String or Hash, and beside a program class that defines index.
def t(l)
  r = yield
  puts [l, r].inspect
rescue Exception => e
  puts [l, e.class].inspect
end
a = [3, 4, 5]
t(:typed_map) { a.index.map { |x| x * 2 } }
t(:typed_rindex_map) { a.rindex.map { |x| x * 2 } }
t(:typed_find_index_map) { a.find_index.map { |x| x * 2 } }
t(:typed_find) { a.index.find { |x| x > 3 } }
t(:typed_any) { a.find_index.any? { |x| x == 5 } }
t(:typed_find_index_with_index) { a.find_index.with_index { |x, i| x == 4 } }
t(:typed_with_index_offset) { a.find_index.with_index(1) { |x, i| i == 3 } }
t(:typed_each_with_index) { a.find_index.each_with_index { |x, i| x * i > 3 } }
t(:typed_with_object) { a.index.with_object([]) { |x, acc| acc << x; false } }
s = ["b", "a"]
t(:typed_str_map) { s.index.map { |x| x.upcase } }
def mk(i) = i == 0 ? [3, 4] : (i == 1 ? +"s" : {k: 1})
t(:boxed_map) { mk(0).index.map { |x| x * 2 } }
t(:boxed_rindex_map) { mk(0).rindex.map { |x| x * 2 } }
t(:boxed_find_index_map) { mk(0).find_index.map { |x| x * 2 } }
t(:boxed_find) { mk(0).index.find { |x| x > 3 } }
t(:boxed_any) { mk(0).index.any? { |x| x == 4 } }
t(:boxed_str) { mk(1).index.map { |x| x } }
t(:boxed_str_rindex) { mk(1).rindex.map { |x| x } }
t(:boxed_hash_find_index) { mk(2).find_index.map { |k, v| v } }
t(:boxed_hash_index) { mk(2).index.map { |x| x } }
class Listing
  def index = [5, 6]
end
def pick(i) = i == 0 ? Listing.new : mk(i - 1)
t(:class_own) { pick(0).index.map { |x| x * 2 } }
t(:class_arr) { pick(1).index.map { |x| x * 2 } }
t(:class_arr_find) { pick(1).index.find { |x| x == 4 } }
t(:class_str) { pick(2).index.map { |x| x } }
t(:class_hash) { pick(3).index.map { |x| x } }

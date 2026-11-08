# A call with no argument on a boxed Array, Hash or String beside a program class
# that defines the same name (merge, values_at, union, difference, intersection,
# zip, index, rindex, find_index, sort_by, slice, start_with?): the Array, Hash or
# String answers, and the class's own instance its own method.
def t(l)
  r = yield
  puts [l, r].inspect
rescue Exception => e
  puts [l, e.class].inspect
end
class U
  %i[merge values_at union difference intersection zip index rindex find_index sort_by slice start_with?].each { |m| define_method(m) { |*a| [:u, m] } }
end
def ua(f) = f ? [3, 1, 2] : U.new
def uh(f) = f ? {a: 1} : U.new
def us(f) = f ? +"ab" : U.new
t(:p10_merge) { uh(true).merge }
t(:p10_merge_u) { uh(false).merge }
t(:p10_values_at) { ua(true).values_at }
t(:p10_union) { ua(true).union }
t(:p10_difference) { ua(true).difference }
t(:p10_intersection) { ua(true).intersection }
t(:p10_zip) { ua(true).zip }
t(:p10_index) { ua(true).index }
t(:p10_sort_by) { ua(true).sort_by }
t(:p10_slice) { ua(true).slice }
t(:p10_start_with) { us(true).start_with? }
t(:p10_u_union) { ua(false).union }

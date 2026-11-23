# map.with_index whose block can answer values of different kinds collects
# them boxed (infer_map_block_ty joins a valued next with the tail), and each
# value keeps its kind: a next of another kind is not cut to the tail's

def fnext(a) = a.map.with_index { |x, i| next 0.5 if i == 0; x }
def fnext1(a) = a.map.with_index(1) { |x, i| next 0.5 if i == 1; x + i }
def inext(a) = a.map.with_index { |x, i| next i if i == 0; x * 1.5 }
def snext(a) = a.collect.with_index { |x, i| next "s" if i == 0; x }
Pt = Struct.new(:x)
def onext(a) = a.map.with_index { |x, i| next 0 if i == 0; Pt.new(x) }

# a boxed next into an Integer tail did not build
def bnext(f)
  _, b = f ? [1, 2.5] : [3, 4]
  [10, 20, 30].map.with_index { |x, i| next b if i == 1; x + i }
end

# a next of the tail's kind keeps the typed Array, and so does a nil one,
# which the Array holds as its nil (a nil next raised TypeError)
def same(a) = a.map.with_index { |x, i| next -1 if i == 0; x + i }
def nilnext(a) = a.map.with_index { |x, i| next nil if i == 0; x + i }
def fnilnext(a) = a.map.with_index(1) { |x, i| next nil if i == 2; x * 0.5 }
# and a tail that can answer nil (it raised TypeError)
def niltail(a) = a.map.with_index { |x, i| x > 3 ? nil : x + i }
def fniltail(a) = a.map.with_index(1) { |x, i| i == 1 ? nil : x * 0.5 }

p fnext([10, 20]), fnext1([10, 20]), inext([1, 2]), snext([1, 2]), onext([5, 6])
p bnext(true), bnext(false)
p same([3, 4]), nilnext([3, 4]), fnilnext([3, 4, 5]), niltail([3, 4]), fniltail([3, 4]), same([3, 4]).sum, fnext([10, 20]).sum, nilnext([3, 4]).compact.sum
p inext([]), snext([7])

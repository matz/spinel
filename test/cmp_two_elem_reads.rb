# `keys[i] <=> keys[j]` on an Integer or Float array that may hold nil, as a
# comparator reads it: two numbers compare, a NaN answers nil, nil <=> nil
# is 0, a nil against a number is nil (and a sort then raises CRuby's
# comparison error), and an index past the end reads nil.
def show(tag)
  r = yield
  puts "#{tag} #{r.inspect}"
rescue => e
  puts "#{tag} #{e.class}: #{e.message}"
end

def cmpf(keys, i, j) = keys[i] <=> keys[j]
def cmpi(keys, i, j) = keys[i] <=> keys[j]
def byf(keys, pos) = pos.sort { |a, b| c = keys[a] <=> keys[b]; c == 0 ? a <=> b : c }
def byi(keys, pos) = pos.sort { |a, b| c = keys[a] <=> keys[b]; c == 0 ? a <=> b : c }

f = [2.5, 0.5, 1.5]
fn = [2.5]
fn[2] = 1.5                  # fn[1] is nil
fn[4] = 0.25                 # and fn[3]
i = [3, 1, 2]
inil = [3]
inil[2] = 2                  # inil[1] is nil
show("float") { [cmpf(f, 0, 1), cmpf(f, 1, 2), cmpf(f, 2, 2)] }
show("float nil") { [cmpf(fn, 0, 1), cmpf(fn, 1, 3), cmpf(fn, 1, 1), cmpf(fn, 3, 4)] }
show("float nan") { cmpf([0.0 / 0.0, 1.0], 0, 1) }
show("float past end") { [cmpf(f, 0, 9), cmpf(f, 9, 9)] }
show("int") { [cmpi(i, 0, 1), cmpi(i, 1, 1)] }
show("int nil") { [cmpi(inil, 0, 1), cmpi(inil, 1, 1)] }
show("sort float") { byf(f, [0, 1, 2]) }
show("sort float nil") { byf(fn, [0, 1, 2]) }
show("sort int") { byi(i, [0, 1, 2]) }
show("sort int nil") { byi(inil, [0, 1, 2]) }

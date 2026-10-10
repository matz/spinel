# An argument that can be nil (a read past an Integer array's end), held in
# a temp ahead of a call because another argument runs code first, keeps its
# nil: the temp was a plain Integer and raised TypeError at the call, where
# the callee's own operator raises NoMethodError for the nil.
module F
  def self.add(a, b) = (a + b) % 97
end
def show
  r = yield
  puts r.inspect
rescue => e
  puts "#{e.class}: #{e.message}"
end
xs = [3, 4]
ys = [5]
show { F.add(ys[1], xs[0]) }
show { F.add(xs[0], xs[1]) }

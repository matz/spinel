# An Array that is an element of what a block or proc parameter receives
# (`|m| m[0] << 1`) widens where it is built when the body stores another
# kind into it, whichever site binds the parameter (a yield, a block's
# call, a forward through &b or super, a proc or lambda call) and at
# whichever place (a required, an optional, a post, a keyword, a value
# taken apart from a splat).

def y1(a) = yield(a)
def y2(a, b) = yield(a, b)
def yk(a) = yield(k: a)
def ys(*a) = yield(*a)
def cl(a, &b) = b.call(a)
def f1(a, &b) = y1(a, &b)
def f2(a, &b) = f1(a, &b)

b = [[+"m"]]; y1(b) { |m| m[0] << 1 }; p b
b = [[+"m"]]; y1(b) { |m = nil| m[0].push(2.5) }; p b
b = [[+"m"]]; y2(0, b) { |i, *r, m| m[0] << i }; p b
b = [[+"m"]]; yk(b) { |k:| k[0].insert(0, :q) }; p b
b = [[+"m"]]; ys(b, 1) { |m, i| m[-1][1] = i }; p b
b = [[+"m"]]; ys(*[b]) { |m| m[0] << 3 }; p b
b = [[+"m"]]; cl(b) { |m| m.first << 5 }; p b
b = [[+"m"]]; f2(b) { |m| m[0].concat([6]) }; p b
pe = proc { |m| m[0] << 8 }; b = [[+"m"]]; pe.call(b); p b
le = ->(i, m = []) { m[0].unshift(i) }; b = [[+"m"]]; le.(10, b); p b
def ykw(**h) = yield(**h)
b = [[+"m"]]; ykw(k: b) { |k:| k[0] << 19 }; p b
b = [[+"m"]]; o = { k: b }; ykw(**o) { |k: nil| k[0] << 20 }; p b

# two blocks storing different kinds, two in one statement, a value that
# may be nil, a Hash's value
b = [[+"m"]]; c = [[+"n"]]
y1(b) { |m| m[0] << 11 }
y1(c) { |m| m[0] << :s }
p [b, c]
b = [[+"m"]]; c = [[+"n"]]
p [y1(b) { |m| m[0] << 18; m }, y1(c) { |m| m[0] << 1.5; m }]
d = ARGV.size > 5 ? nil : [[+"m"]]
y1(d) { |m| m[0] << 12 if m }
p d
h = { a: [+"m"] }
y1(h) { |t| t[:a] << 13 }
p h

# a block in tail position, and the block's value
def tail(v) = y1(v) { |m| m[0].push(:t) }
b = [[+"m"]]; tail(b); p b
b = [[+"m"]]; p(y1(b) { |m| m[0] << 17; m }.equal?(b)); p b

class P1; def run(a) = yield(a); end
class S4 < P1; def run(a, &b) = super; end
b = [[+"m"]]; S4.new.run(b) { |m| m[0] << 7 }; p b

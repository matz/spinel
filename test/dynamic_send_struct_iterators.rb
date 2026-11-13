# A computed name reaches the iterators a Struct generates (each, each_pair,
# each_with_index), through send, public_send and method, on a Struct, a
# keyword_init Struct and a subclass of one.
S = Struct.new(:a, :b)
K = Struct.new(:x, :y, keyword_init: true)
class Sub < Struct.new(:p, :q); end

def fwd(o, n, &b) = o.send(n, &b)

[S.new(1, 2), K.new(x: 3, y: 4), Sub.new(5, 6)].each do |o|
  %w[each each_pair each_with_index].each do |n|
    r = []
    o.send(n) { |*v| r << v }
    p r
    r = []
    o.public_send(n.to_sym) { |*v| r << v }
    p r
    r = []
    fwd(o, n) { |*v| r << v }
    p r
    r = []
    o.method(n).call { |*v| r << v }
    p r
  end
end

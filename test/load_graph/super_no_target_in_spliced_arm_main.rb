# A `super` no ancestor answers raises NoMethodError where it runs. As the arm of a conditional
# whose other arm is an Array, in a method that takes a block (spliced at its call) or a class-side
# method_missing a late version reaches, its raise token takes the arm's slot.
module Q; def self.setup = (def j(&b) = [:q, b&.call]); end
class K; end
class S < K; end
class << S; def method_missing(n, *a, &b) = n == :j ? [:smm, b&.call] : super; end
class K; singleton_class.prepend Q; end
def t = yield rescue [$!.class]
p [t { S.j { 1 } }, t { K.j { 2 } }]
Q.setup
p [t { S.j { 1 } }, t { K.j { 2 } }]

class Hold; end
class Under < Hold; end
class << Hold; def method_missing(n, *a, **kw, &b) = n == :j ? [:hmm, a, kw, b&.call] : super; end
p t { Hold.method_missing(:j, 1, k: 2) { 3 } }
p t { Under.method_missing(:other) { 3 } }
p t { Hold.method_missing(:other) }

class Blk
  def bar(n, &b) = n ? [:c, b&.call] : super
  def self.cbar(n, &b) = n ? [:cc, b&.call] : super
end
p [t { Blk.new.bar(true) { 1 } }, t { Blk.new.bar(false) { 1 } }]
p [t { Blk.cbar(true) { 1 } }, t { Blk.cbar(false) { 1 } }]
